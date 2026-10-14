#include "project_files_internal.h"

/// A row of the tree: a chevron and folder for a folder, a page for a file, then its name.
typedef struct { char *name; int depth; bool folder, open, selected; } RowData;

static void tree_done(void *owner, Request *req);
static void branches_done(void *owner, Request *req);
static void row_free(void *v);
static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void set_ref(ProjectFiles *p, const char *ref);

/// The ref files are read at: the commit the tree was read at, else the branch picked.
const char *project_files_files_ref(const ProjectFiles *p) {
    if (p->has_tree) return p->tree.sha ? p->tree.sha : p->tree.ref;
    return p->ref;
}

const char *project_files_shown_ref(const ProjectFiles *p) {
    if (p->ref) return p->ref;
    if (p->has_tree && !str_empty(p->tree.ref)) return p->tree.ref;
    return p->default_branch ? p->default_branch : "default branch";
}

// MARK: - Saved state

// The branch and the open tabs are kept on disk per repository, as the other tabs' pickers are.
char *project_files_state_key(const ProjectFiles *p) { return xstrfmt("repo-files:%s", p->repo); }

void project_files_state_save(const ProjectFiles *p) {
    Json *saved = json_object();
    if (p->ref) json_set_str(saved, "ref", p->ref);
    Json *tabs = json_array();
    for (size_t i = 0; i < p->tab_count; i++) json_array_push(tabs, json_string(p->tabs[i]->path));
    json_object_set(saved, "tabs", tabs);
    json_set_num(saved, "active", p->active);
    json_set_num(saved, "mode", p->mode);
    char *key = project_files_state_key(p); cache_store(g_store.cache, saved, key); free(key);
    json_free(saved);
}

static void tree_done(void *owner, Request *req) {
    ProjectFiles *p = owner;
    p->loaded = true;
    if (!req->ok) { request_error_into(&p->error, req); project_files_relayout(p); return; }
    RepoTree tree;
    if (!repo_tree_parse(req->result, &tree)) { set_string(&p->error, "The server returned an unexpected response."); project_files_relayout(p); return; }
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
    FileTab *f = project_files_active_file(p);
    if (f) repo_tree_reveal(&p->tree, f->path);
    project_files_find_update(p);
    // The open file is read again when the tree is at another commit; the other tabs when they are shown.
    project_files_file_ensure(p, f, true);
    project_files_index_sync(p);
    project_files_relayout(p);
}

void project_files_load(ProjectFiles *p) {
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
    project_files_relayout(p);
}

void project_files_open(ProjectFiles *p) {
    if (!p->loaded && !p->req) project_files_load(p);
    if (!p->branches_read && !p->req_branches && store_supports("branches")) {
        Json *args = json_object(); json_set_str(args, "repo", p->repo);
        store_call("branches", args, 0, p, branches_done, 0, &p->req_branches);
    }
}

void project_files_refresh(ProjectFiles *p) {
    // Every tab is read again once the tree says at which commit, failed ones included.
    for (size_t i = 0; i < p->tab_count; i++) { set_string(&p->tabs[i]->error, NULL); set_string(&p->tabs[i]->read_at, NULL); }
    p->branches_read = false;
    project_files_load(p);
    project_files_open(p);
    project_files_relayout(p);
}

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

void project_files_layout_tree(ProjectFiles *p, Doc *doc, int x, int w) {
    const char *selected = project_files_active_file(p) ? project_files_active_file(p)->path : NULL;
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

// MARK: - Actions

static void set_ref(ProjectFiles *p, const char *ref) {
    if (str_eq(ref, p->ref)) return;
    project_files_index_drop(p);
    set_string(&p->ref, ref);
    project_files_state_save(p);
    project_files_find_clear(p);
    project_files_load(p);
    project_files_relayout(p);
}

void project_files_pick_branch(ProjectFiles *p, POINT pt) {
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
