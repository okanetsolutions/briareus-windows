// A project's Files tab: its repository as PhpStorm's project view shows it, read through the server's GitHub token
// (`repo_tree`, `repo_file`), so no checkout or token of the user's own is needed. The tree is on the left, folders
// first, at the branch the header's picker names (the default one until another is picked). Go to File (Ctrl+P) over
// it finds a file by a few of its letters. Files open as tabs on the right, each read at the commit the tree was read
// at, so what opens matches the tree shown while the branch moves on; their lines are numbered and coloured by
// language, and select and copy as text. A binary file, or one over 1 MB, shows its size and a link to GitHub.
#include "repo.h"
#include "screens.h"
#include "sftp.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_ROW, A_RESULT, A_TAB, A_TAB_CLOSE, A_BRANCH, A_OPEN_GITHUB, A_COPY, A_RETRY, A_FOCUS_FIND, A_CLEAR_FIND, A_RETRY_FILE };
enum { ID_FIND = 0x5F30 };          // the Go to File edit's control id
enum { MAX_TABS = 20, MAX_RESULTS = 50, GUTTER_BLOCK = 64 };

/// A file open as a tab, and its lines once read.
typedef struct ProjectFiles ProjectFiles;
typedef struct {
    ProjectFiles *owner;
    char *path;
    RepoFile file; bool loaded;     // `file` holds the server's answer
    char *read_at;                  // the ref it was read at; a tree read at another commit reads it again
    char *error;
    Request *req;
    // Worked out once per answer: the lines, and each one's runs (line i's are tokens[first[i]] up to tokens[first[i + 1]]).
    RepoLine *lines; size_t line_count;
    CodeToken *tokens; size_t *first;
    size_t widest;                  // the longest line, in columns
} FileTab;

struct ProjectFiles {
    char *repo;
    Screen *host;
    int base;
    char *ref;                      // the branch picked; NULL for the default one
    RepoTree tree; bool has_tree;
    bool loaded;                    // the server answered since the tab was first opened
    char *error;
    Request *req;
    // The branch picker's choices, read once.
    char **branches; size_t branch_count; char *default_branch; bool branches_read; Request *req_branches;
    FileTab **tabs; size_t tab_count; int active;   // -1 for none
    // Go to File: its edit, where the last layout put it, and what it finds.
    HWND find; RECT find_rc; bool find_laid, clipped;
    char *query; size_t *results; size_t result_count; int pick;
};

bool project_files_offered(void) { return store_supports("repo_tree") && store_supports("repo_file"); }

static void relayout(ProjectFiles *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static FileTab *active_file(ProjectFiles *p) { return p->active >= 0 && (size_t)p->active < p->tab_count ? p->tabs[p->active] : NULL; }
/// The ref files are read at: the commit the tree was read at, else the branch picked.
static const char *files_ref(const ProjectFiles *p) {
    if (p->has_tree) return p->tree.sha ? p->tree.sha : p->tree.ref;
    return p->ref;
}
static const char *shown_ref(const ProjectFiles *p) {
    if (p->ref) return p->ref;
    if (p->has_tree && !str_empty(p->tree.ref)) return p->tree.ref;
    return p->default_branch ? p->default_branch : "default branch";
}

// MARK: - Saved state

// The branch and the open tabs are kept on disk per repository, as the other tabs' pickers are.
static char *state_key(const ProjectFiles *p) { return xstrfmt("repo-files:%s", p->repo); }
static void state_save(const ProjectFiles *p) {
    Json *saved = json_object();
    if (p->ref) json_set_str(saved, "ref", p->ref);
    Json *tabs = json_array();
    for (size_t i = 0; i < p->tab_count; i++) json_array_push(tabs, json_string(p->tabs[i]->path));
    json_object_set(saved, "tabs", tabs);
    json_set_num(saved, "active", p->active);
    char *key = state_key(p); cache_store(g_store.cache, saved, key); free(key);
    json_free(saved);
}

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
static void file_free(FileTab *f) {
    if (!f) return;
    request_cancel(&f->req);
    repo_file_free(&f->file); file_forget_lines(f);
    free(f->path); free(f->read_at); free(f->error);
    free(f);
}

static void file_done(void *owner, Request *req) {
    FileTab *f = owner;
    ProjectFiles *p = f->owner;
    if (!req->ok) { request_error_into(&f->error, req); relayout(p); return; }
    RepoFile file;
    if (!repo_file_parse(req->result, &file)) { set_string(&f->error, "The server returned an unexpected response."); relayout(p); return; }
    repo_file_free(&f->file);
    f->file = file; f->loaded = true;
    set_string(&f->error, NULL);
    set_string(&f->read_at, json_str(json_get(req->args, "ref")));
    file_prepare(f);
    relayout(p);
}
/// Reads the file when it has not been read at the tree's commit; a failed read waits for Try again.
static void file_ensure(ProjectFiles *p, FileTab *f, bool retry) {
    if (!f || f->req || (!p->has_tree && !p->loaded)) return;
    if (f->error && !retry) return;
    const char *ref = files_ref(p);
    if (f->loaded && str_eq(f->read_at, ref)) return;
    set_string(&f->error, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    json_set_str(args, "path", f->path);
    if (ref) json_set_str(args, "ref", ref);
    store_call("repo_file", args, 0, f, file_done, 0, &f->req);
}

static void activate(ProjectFiles *p, int index) {
    if (index < 0 || (size_t)index >= p->tab_count) { p->active = -1; return; }
    p->active = index;
    if (p->has_tree) repo_tree_reveal(&p->tree, p->tabs[index]->path);
    file_ensure(p, p->tabs[index], false);
}
static FileTab *tab_add(ProjectFiles *p, const char *path) {
    FileTab *f = xcalloc(1, sizeof *f);
    f->owner = p; f->path = xstrdup(path);
    p->tabs = xrealloc(p->tabs, (p->tab_count + 1) * sizeof *p->tabs);
    p->tabs[p->tab_count++] = f;
    return f;
}
static void tab_close(ProjectFiles *p, size_t index) {
    if (index >= p->tab_count) return;
    file_free(p->tabs[index]);
    memmove(&p->tabs[index], &p->tabs[index + 1], (p->tab_count - index - 1) * sizeof *p->tabs);
    p->tab_count--;
    // The tab to the right takes its place, as an editor's do; the last one's left neighbour.
    if (p->active > (int)index) p->active--;
    else if (p->active == (int)index) p->active = p->tab_count ? (int)(index < p->tab_count ? index : p->tab_count - 1) : -1;
    activate(p, p->active);
}
/// Opens a file as a tab, or shows the tab it already has; past MAX_TABS the oldest other tab closes.
static void open_path(ProjectFiles *p, const char *path) {
    for (size_t i = 0; i < p->tab_count; i++) if (str_eq(p->tabs[i]->path, path)) { activate(p, (int)i); state_save(p); relayout(p); return; }
    if (p->tab_count >= (size_t)MAX_TABS) tab_close(p, p->active == 0 ? 1 : 0);
    tab_add(p, path);
    activate(p, (int)p->tab_count - 1);
    state_save(p);
    if (p->host->pane) pane_scroll_to_top(p->host->pane);
    relayout(p);
}

// MARK: - The tree

static void find_update(ProjectFiles *p);
static void tree_done(void *owner, Request *req) {
    ProjectFiles *p = owner;
    p->loaded = true;
    if (!req->ok) { request_error_into(&p->error, req); relayout(p); return; }
    RepoTree tree;
    if (!repo_tree_parse(req->result, &tree)) { set_string(&p->error, "The server returned an unexpected response."); relayout(p); return; }
    set_string(&p->error, NULL);
    // The folders unfolded stay so in the tree read again, or at another branch where it has them.
    if (p->has_tree) {
        for (size_t i = 0; i < p->tree.count; i++) {
            if (!p->tree.entries[i].open) continue;
            int k = repo_tree_find(&tree, p->tree.entries[i].path);
            if (k >= 0) tree.entries[k].open = true;
        }
        repo_tree_free(&p->tree);
    }
    p->tree = tree; p->has_tree = true;
    FileTab *f = active_file(p);
    if (f) repo_tree_reveal(&p->tree, f->path);
    find_update(p);
    // The open file is read again when the tree is at another commit; the other tabs when they are shown.
    file_ensure(p, f, true);
    relayout(p);
}
static void load(ProjectFiles *p) {
    if (!project_files_offered()) return;
    request_cancel(&p->req);
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    if (p->ref) json_set_str(args, "ref", p->ref);
    store_call("repo_tree", args, 0, p, tree_done, 0, &p->req);
}
static void branches_done(void *owner, Request *req) {
    ProjectFiles *p = owner;
    // Read or refused, it is not asked again until the refresh: the picker then offers the default branch alone.
    p->branches_read = true;
    if (!req->ok) return;
    str_array_free(p->branches, p->branch_count); p->branches = NULL; p->branch_count = 0;
    p->branches = json_dup_strings(json_get(req->result, "branches"), &p->branch_count);
    set_string(&p->default_branch, json_str(json_get(req->result, "defaultBranch")));
    relayout(p);
}
void project_files_open(ProjectFiles *p) {
    if (!p->loaded && !p->req) load(p);
    if (!p->branches_read && !p->req_branches && store_supports("branches")) {
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("branches", args, 0, p, branches_done, 0, &p->req_branches);
    }
}
void project_files_refresh(ProjectFiles *p) {
    // Every tab is read again once the tree says at which commit, failed ones included.
    for (size_t i = 0; i < p->tab_count; i++) { set_string(&p->tabs[i]->error, NULL); set_string(&p->tabs[i]->read_at, NULL); }
    p->branches_read = false;
    load(p);
    project_files_open(p);
    relayout(p);
}

// MARK: - Go to File

static void find_update(ProjectFiles *p) {
    free(p->results); p->results = NULL; p->result_count = 0;
    if (p->has_tree && !str_empty(p->query)) p->results = repo_find_files(&p->tree, p->query, MAX_RESULTS, &p->result_count);
    if (p->pick >= (int)p->result_count) p->pick = p->result_count ? (int)p->result_count - 1 : 0;
    if (p->pick < 0) p->pick = 0;
}
static void find_clear(ProjectFiles *p) {
    if (p->find) SetWindowTextW(p->find, L"");   // its EN_CHANGE clears the query
    set_string(&p->query, NULL); find_update(p);
}
static void open_result(ProjectFiles *p, size_t k) {
    if (k >= p->result_count) return;
    char *path = xstrdup(p->tree.entries[p->results[k]].path);
    find_clear(p);
    if (p->host->pane) SetFocus(pane_hwnd(p->host->pane));
    open_path(p, path);
    free(path);
}
static LRESULT CALLBACK find_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    ProjectFiles *p = (ProjectFiles *)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_DOWN || wp == VK_UP) {
            int n = (int)p->result_count;
            if (n) { p->pick = (p->pick + (wp == VK_DOWN ? 1 : n - 1)) % n; relayout(p); }
            return 0;
        }
        if (wp == VK_RETURN) { open_result(p, (size_t)p->pick); return 0; }
        if (wp == VK_ESCAPE) { find_clear(p); SetFocus(GetParent(hwnd)); relayout(p); return 0; }
        if (wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\r' || wp == 0x1B || wp == 0x01) return 0;
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
    SendMessageW(p->find, EM_SETCUEBANNER, TRUE, (LPARAM)L"Go to file (Ctrl+P)");
    SetWindowSubclass(p->find, find_proc, 0, (DWORD_PTR)p);
    theme_apply_control(p->find);
}
bool project_files_command(ProjectFiles *p, int id, int code) {
    if (id != ID_FIND || !p->find) return false;
    if (code == EN_CHANGE) {
        int len = GetWindowTextLengthW(p->find);
        wchar_t *w = xcalloc((size_t)len + 1, sizeof *w);
        GetWindowTextW(p->find, w, len + 1);
        char *text = wide_to_utf8(w); free(w);
        set_string(&p->query, str_empty(text) ? NULL : text);
        free(text);
        p->pick = 0;
        find_update(p);
        relayout(p);
    }
    return true;
}
bool project_files_key(ProjectFiles *p, WPARAM vk, bool ctrl, bool shift) {
    // Ctrl+P, or Ctrl+Shift+O as PhpStorm's Go to File on a Mac.
    if (!ctrl || !((vk == 'P' && !shift) || (vk == 'O' && shift)) || !p->find) return false;
    if (p->host->pane) pane_scroll_to_top(p->host->pane);
    SetFocus(p->find);
    SendMessageW(p->find, EM_SETSEL, 0, -1);
    return true;
}
void project_files_place(ProjectFiles *p, const RECT *content, int scroll_y, bool shown) {
    if (!p->find) return;
    if (!shown || !content || !p->find_laid) { ShowWindow(p->find, SW_HIDE); return; }
    RECT rc; GetClientRect(pane_hwnd(p->host->pane), &rc);
    int m = (rc.right - rc.left - pane_content_width(p->host->pane)) / 2;
    RECT r = { content->left + m + p->find_rc.left, content->top + p->find_rc.top - scroll_y, content->left + m + p->find_rc.right, content->top + p->find_rc.bottom - scroll_y };
    RECT visible;
    if (!IntersectRect(&visible, &r, content)) { ShowWindow(p->find, SW_HIDE); return; }
    MoveWindow(p->find, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    bool clipped = !EqualRect(&visible, &r);
    if (clipped) SetWindowRgn(p->find, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
    else if (p->clipped) SetWindowRgn(p->find, NULL, TRUE);
    p->clipped = clipped;
    ShowWindow(p->find, SW_SHOWNA);
}

// MARK: - Painting

/// The Go to File box; the edit sits over it.
typedef struct { bool focused; } FindData;
static void paint_find(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    const FindData *d = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, d->focused ? theme.accent_dim : theme.line);
    RECT g = { rc->left + px(10), rc->top, rc->left + px(26), rc->bottom };
    draw_glyph(cv, 0xE721, &g, FONT_ICON_SMALL, theme.muted);
}
static void paint_clear(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT g = *rc;
    draw_glyph(cv, 0xE711, &g, FONT_ICON_SMALL, doc_item_hovered(doc, it) ? theme.ink : theme.muted);
}

/// A row of the tree: a chevron and folder for a folder, a page for a file, then its name.
typedef struct { char *name; int depth; bool folder, open, selected; } RowData;
static void row_free(void *v) { RowData *d = v; free(d->name); free(d); }
static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    const RowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->selected || hovered) {
        COLORREF fill = d->selected ? blend(theme.accent, theme.canvas, 0.16) : theme.raise;
        fill_round_rect(cv, rc, px(6), fill, fill);
    }
    int x = rc->left + px(6) + d->depth * px(16);
    if (d->folder) {
        RECT c = { x, rc->top, x + px(14), rc->bottom }; draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &c, FONT_ICON_SMALL, theme.muted);
        x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8B7, &g, FONT_ICON_SMALL, theme.accent);
    } else {
        x += px(16);
        RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8A5, &g, FONT_ICON_SMALL, theme.muted);
    }
    x += px(20);
    RECT t = { x, rc->top, rc->right - px(6), rc->bottom };
    draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/// A file Go to File found: its name, then the folder it is in.
typedef struct { char *name, *folder; bool picked; } ResultData;
static void result_free(void *v) { ResultData *d = v; free(d->name); free(d->folder); free(d); }
static void paint_result(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    const ResultData *d = it->data;
    if (d->picked || doc_item_hovered(doc, it)) {
        COLORREF fill = d->picked ? blend(theme.accent, theme.canvas, 0.16) : theme.raise;
        fill_round_rect(cv, rc, px(6), fill, fill);
    }
    int x = rc->left + px(8);
    RECT g = { x, rc->top, x + px(16), rc->bottom }; draw_glyph(cv, 0xE8A5, &g, FONT_ICON_SMALL, theme.muted);
    x += px(22);
    int nw = text_width(cv, d->name, FONT_FOOTNOTE_SEMIBOLD);
    RECT n = { x, rc->top, rc->right - px(6), rc->bottom };
    draw_text(cv, d->name, &n, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (!str_empty(d->folder) && x + nw + px(8) < rc->right - px(6)) {
        RECT f = { x + nw + px(8), rc->top, rc->right - px(6), rc->bottom };
        draw_text(cv, d->folder, &f, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
}

/// An editor tab: the file's name, lit when it is the one shown; its ✕ is an item of its own over it.
typedef struct { char *name; bool active; } TabData;
static void tab_data_free(void *v) { TabData *d = v; free(d->name); free(d); }
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    const TabData *d = it->data;
    COLORREF fill = d->active ? theme.raise : doc_item_hovered(doc, it) ? blend(theme.raise, theme.canvas, 0.5) : theme.canvas;
    fill_round_rect(cv, rc, px(6), fill, d->active ? theme.line : fill);
    if (d->active) { RECT bar = { rc->left + px(6), rc->bottom - px(2), rc->right - px(6), rc->bottom }; fill_rect(cv, &bar, theme.accent); }
    RECT t = { rc->left + px(10), rc->top, rc->right - px(26), rc->bottom };
    draw_text(cv, d->name, &t, d->active ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, d->active ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/// Line numbers for a block of lines, painted together.
typedef struct { size_t first, count; int row_h; } GutterData;
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

// MARK: - Layout

/// The Go to File box at (x, cursor), the edit over it; advances.
static void layout_find(ProjectFiles *p, Doc *doc, int x, int w) {
    find_ensure(p);
    int h = px(34), fh = edit_line_height(FONT_BODY);
    RECT box = { x, doc->y, x + w, doc->y + h };
    FindData *d = xcalloc(1, sizeof *d);
    d->focused = p->find && GetFocus() == p->find;
    Item *it = doc_item(doc, doc_add(doc, &box, paint_find));
    it->data = d; it->free_data = free; it->action = p->base + A_FOCUS_FIND; it->hover_fill = false;
    int clear_w = !str_empty(p->query) ? px(28) : 0;
    p->find_rc = (RECT){ x + px(32), box.top + (h - fh) / 2, x + w - px(10) - clear_w, box.top + (h - fh) / 2 + fh };
    p->find_laid = true;
    if (clear_w) {
        RECT c = { x + w - px(8) - clear_w, box.top, x + w - px(8), box.bottom };
        Item *ci = doc_item(doc, doc_add(doc, &c, paint_clear));
        ci->action = p->base + A_CLEAR_FIND; ci->hand = true; ci->tip = xstrdup("Clear");
    }
    doc->y = box.bottom;
}

static void layout_results(ProjectFiles *p, Doc *doc, int x, int w) {
    if (!p->result_count) { doc_text(doc, x + px(8), w - px(16), "No file matches.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE); return; }
    for (size_t k = 0; k < p->result_count; k++) {
        const RepoEntry *e = &p->tree.entries[p->results[k]];
        ResultData *d = xcalloc(1, sizeof *d);
        d->name = xstrdup(e->name);
        d->folder = xstrndup(e->path, (size_t)(e->name - e->path) ? (size_t)(e->name - e->path) - 1 : 0);
        d->picked = (int)k == p->pick;
        int i = doc_custom(doc, x, w, px(26), paint_result, d, result_free, p->base + A_RESULT, (intptr_t)k);
        doc_item(doc, i)->hover_fill = false;
        doc_item(doc, i)->tip = xstrdup(e->path);
    }
    if (p->result_count == (size_t)MAX_RESULTS) { doc_space(doc, px(4)); doc_text(doc, x + px(8), w - px(16), "Only the 50 best matches are listed.", FONT_CAPTION2, theme.tertiary, DT_LEFT | DT_WORDBREAK); }
}

static void layout_tree(ProjectFiles *p, Doc *doc, int x, int w) {
    const char *selected = active_file(p) ? active_file(p)->path : NULL;
    if (!p->tree.count) { doc_text(doc, x + px(8), w - px(16), "This branch has no files.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE); return; }
    for (size_t i = 0; i < p->tree.count;) {
        const RepoEntry *e = &p->tree.entries[i];
        RowData *d = xcalloc(1, sizeof *d);
        d->name = xstrdup(e->name); d->depth = e->depth; d->folder = e->folder; d->open = e->open;
        d->selected = !e->folder && str_eq(e->path, selected);
        int k = doc_custom(doc, x, w, px(24), paint_row, d, row_free, p->base + A_ROW, (intptr_t)i);
        doc_item(doc, k)->hover_fill = false;
        i = e->folder && !e->open ? repo_tree_skip(&p->tree, i) : i + 1;
    }
    if (p->tree.truncated) {
        doc_space(doc, px(8));
        doc_text(doc, x + px(8), w - px(16), "GitHub lists only the first 100,000 entries of this repository; the rest are left out.", FONT_CAPTION2, theme.warn, DT_LEFT | DT_WORDBREAK);
    }
}

/// The open files as tabs, wrapping; each closes with its ✕, or from its menu (right click).
static void layout_tabs(ProjectFiles *p, Doc *doc, int x, int w) {
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
        Item *ci = doc_item(doc, doc_add(doc, &c, paint_clear));
        ci->action = p->base + A_TAB_CLOSE; ci->arg = (intptr_t)i; ci->hand = true; ci->tip = xstrdup("Close");
        tx += tw + px(4);
    }
    doc->y = ty + h;
}

/// The file shown: its path and size, Copy and Open on GitHub, then its numbered lines, or why it has none.
static void layout_file(ProjectFiles *p, Doc *doc, int x, int w) {
    FileTab *f = active_file(p);
    if (!f) {
        doc_empty_state(doc, x, w, 0xE8A5, "No file open", "Pick a file in the tree, or press Ctrl+P and type a few letters of its name.");
        return;
    }
    file_ensure(p, f, false);
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
    ButtonSpec buttons[2]; size_t bc = 0;
    if (f->file.content) buttons[bc++] = (ButtonSpec){ 0xE8C8, "Copy", BUTTON_PLAIN, p->base + A_COPY, 0, true };
    if (safe_web_url(f->file.url)) buttons[bc++] = (ButtonSpec){ 0xE8A7, "Open on GitHub", BUTTON_PLAIN, p->base + A_OPEN_GITHUB, 0, true };
    if (bc) { doc_button_row(doc, x, w, buttons, bc); doc_space(doc, px(8)); }
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
        doc_code_line(doc, &rc, text + f->lines[i].start, f->lines[i].len, spans, n, FONT_MONO_SMALL, advance);
        doc->y += row_h;
    }
    free(spans);
    doc_space(doc, px(6));
    doc_box_end(doc, box, 0);
    // Long lines scroll sideways: the box grows to the widest one.
    if (right > x + w) { doc_item(doc, box)->rc.right = right; if (right > doc->content_width) doc->content_width = right; }
}

void project_files_layout(ProjectFiles *p, Doc *doc, int w) {
    project_files_open(p);
    p->find_laid = false;
    if (p->error) {
        doc_notice(doc, 0, w, p->error);
        doc_space(doc, px(8));
        doc_button(doc, 0, 0, "Try again", BUTTON_BORDERED, p->base + A_RETRY, 0, !p->req);
        doc_space(doc, px(12));
    }
    if (!p->has_tree) {
        if (!p->error) doc_loading(doc, 0, w, "Loading the files\xE2\x80\xA6");
        return;
    }
    bool wide = w >= px(720);
    int tree_w = w * 28 / 100, gap = px(16);
    if (tree_w < px(240)) tree_w = px(240);
    if (tree_w > px(340)) tree_w = px(340);
    if (!wide) tree_w = w;
    // Go to File, and the branch being read when it changes.
    layout_find(p, doc, 0, tree_w);
    if (p->req && p->loaded) {
        char *line = xstrfmt("Reading %s\xE2\x80\xA6", shown_ref(p));
        RECT r = { wide ? tree_w + gap : 0, doc->y - px(34), w, doc->y };
        if (!wide) { doc_space(doc, px(4)); r.top = doc->y; r.bottom = doc->y + px(18); doc->y = r.bottom; }
        doc_text_at(doc, &r, line, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        free(line);
    }
    doc_space(doc, px(10));
    int top = doc->y, first = (int)doc->count;
    if (!str_empty(p->query)) layout_results(p, doc, 0, tree_w); else layout_tree(p, doc, 0, tree_w);
    int last = (int)doc->count, bottom = doc->y;
    if (!wide) {
        doc_space(doc, px(14));
        if (p->tab_count) { layout_tabs(p, doc, 0, w); doc_space(doc, px(10)); }
        layout_file(p, doc, 0, w);
        doc_space(doc, px(16));
        return;
    }
    doc->y = top;
    int fx = tree_w + gap, fw = w - fx;
    if (p->tab_count) { layout_tabs(p, doc, fx, fw); doc_space(doc, px(10)); }
    layout_file(p, doc, fx, fw);
    doc_space(doc, px(16));
    // The tree stays in view beside the file and scrolls on its own, as the changed files' tree does: the page is as
    // long as the file, or the view's height when the tree needs that.
    if (p->host->pane) {
        RECT view = pane_content_rect(p->host->pane);
        int tree_h = bottom - top, room = view.bottom - view.top - px(24);
        if (tree_h > room) tree_h = room;
        if (doc->y < top + tree_h) doc->y = top + tree_h;
    }
    doc_sticky(doc, first, last, doc->y);
}

void project_files_header(ProjectFiles *p, HeaderInfo *info) {
    if (p->has_tree) {
        size_t files = repo_tree_file_count(&p->tree);
        char sha[8] = "";
        if (p->tree.sha) snprintf(sha, sizeof sha, "%.7s", p->tree.sha);
        snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s%s%zu file%s", p->repo, sha, sha[0] ? " \xC2\xB7 " : "", files, files == 1 ? "" : "s");
    }
    // The branch picker, as the other tabs' pickers are.
    if (info->button_count < HEADER_BUTTONS) {
        HeaderButton *b = &info->buttons[info->button_count++];
        snprintf(b->label, sizeof b->label, "\xE2\x8E\x87 %s \xE2\x96\xBE", shown_ref(p));
        b->action = p->base + A_BRANCH; b->enabled = true; b->tip = "Browse another branch";
    }
}

// MARK: - Actions

static void set_ref(ProjectFiles *p, const char *ref) {
    if (str_eq(ref, p->ref)) return;
    set_string(&p->ref, ref);
    state_save(p);
    find_clear(p);
    load(p);
    relayout(p);
}
static void pick_branch(ProjectFiles *p, POINT pt) {
    HMENU menu = CreatePopupMenu();
    char *first = p->default_branch ? xstrfmt("Default branch (%s)", p->default_branch) : xstrdup("Default branch");
    wchar_t *wf = utf8_to_wide(first);
    AppendMenuW(menu, MF_STRING | (!p->ref ? MF_CHECKED : 0), 1, wf);
    free(first); free(wf);
    if (!p->branches_read) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, store_supports("branches") ? L"Loading branches\x2026" : L"No other branches to pick");
    for (size_t i = 0; i < p->branch_count; i++) {
        if (str_eq(p->branches[i], p->default_branch)) continue;
        wchar_t *w = utf8_to_wide(p->branches[i]);
        AppendMenuW(menu, MF_STRING | (str_eq(p->ref, p->branches[i]) ? MF_CHECKED : 0), (UINT_PTR)(2 + i), w);
        free(w);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) set_ref(p, NULL);
    else if (chosen >= 2 && (size_t)(chosen - 2) < p->branch_count) set_ref(p, p->branches[chosen - 2]);
}

bool project_files_action(ProjectFiles *p, int action, intptr_t arg, POINT pt) {
    if (action < p->base || action >= p->base + PROJECT_FILES_ACTIONS) return false;
    FileTab *f = active_file(p);
    switch (action - p->base) {
    case A_ROW:
        if (!p->has_tree || (size_t)arg >= p->tree.count) break;
        if (p->tree.entries[arg].folder) { p->tree.entries[arg].open = !p->tree.entries[arg].open; relayout(p); }
        else open_path(p, p->tree.entries[arg].path);
        break;
    case A_RESULT: open_result(p, (size_t)arg); break;
    case A_TAB: if ((size_t)arg < p->tab_count) { activate(p, (int)arg); state_save(p); relayout(p); } break;
    case A_TAB_CLOSE: tab_close(p, (size_t)arg); state_save(p); relayout(p); break;
    case A_BRANCH: if (p->host->pane) pick_branch(p, pt); break;
    case A_OPEN_GITHUB: if (f && safe_web_url(f->file.url)) open_web_url(f->file.url); break;
    case A_COPY: if (f && f->file.content && p->host->pane) copy_to_clipboard(pane_hwnd(p->host->pane), f->file.content); break;
    case A_RETRY: set_string(&p->error, NULL); load(p); relayout(p); break;
    case A_RETRY_FILE: if (f) { file_ensure(p, f, true); relayout(p); } break;
    case A_FOCUS_FIND: if (p->find) SetFocus(p->find); break;
    case A_CLEAR_FIND: find_clear(p); relayout(p); break;
    }
    return true;
}

/// A tab's menu: close it, the others, or all of them.
bool project_files_context(ProjectFiles *p, int action, intptr_t arg, POINT pt) {
    if ((action != p->base + A_TAB && action != p->base + A_TAB_CLOSE) || (size_t)arg >= p->tab_count || !p->host->pane) return false;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Close");
    AppendMenuW(menu, MF_STRING | (p->tab_count > 1 ? 0 : MF_GRAYED), 2, L"Close others");
    AppendMenuW(menu, MF_STRING, 3, L"Close all");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 4, L"Copy path");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    size_t index = (size_t)arg;
    switch (chosen) {
    case 1: tab_close(p, index); break;
    case 2: {
        FileTab *keep = p->tabs[index];
        for (size_t i = 0; i < p->tab_count; i++) if (p->tabs[i] != keep) file_free(p->tabs[i]);
        p->tabs[0] = keep; p->tab_count = 1;
        activate(p, 0);
        break;
    }
    case 3:
        for (size_t i = 0; i < p->tab_count; i++) file_free(p->tabs[i]);
        p->tab_count = 0; p->active = -1;
        break;
    case 4: copy_to_clipboard(pane_hwnd(p->host->pane), p->tabs[index]->path); break;
    }
    if (chosen >= 1 && chosen <= 3) { state_save(p); relayout(p); }
    return true;
}

// MARK: - Lifetime

ProjectFiles *project_files_new(const char *repo, Screen *host, int action_base) {
    ProjectFiles *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo); p->host = host; p->base = action_base; p->active = -1;
    // The branch and tabs open last time; each file is read once the tree says at which commit.
    char *key = state_key(p);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    set_string(&p->ref, json_str_nonempty(json_get(saved, "ref")));
    const Json *tabs = json_get(saved, "tabs");
    for (size_t i = 0; i < json_count(tabs) && p->tab_count < (size_t)MAX_TABS; i++) { const char *path = json_str_nonempty(json_at(tabs, i)); if (path) tab_add(p, path); }
    int active = json_int_or(json_get(saved, "active"), 0);
    p->active = p->tab_count ? (active >= 0 && (size_t)active < p->tab_count ? active : 0) : -1;
    json_free(saved);
    return p;
}
void project_files_free(ProjectFiles *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->req_branches);
    for (size_t i = 0; i < p->tab_count; i++) file_free(p->tabs[i]);
    free(p->tabs);
    if (p->find) DestroyWindow(p->find);
    if (p->has_tree) repo_tree_free(&p->tree);
    str_array_free(p->branches, p->branch_count);
    free(p->repo); free(p->ref); free(p->error); free(p->default_branch); free(p->query); free(p->results);
    free(p);
}
