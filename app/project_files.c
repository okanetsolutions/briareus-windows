// A project's Files tab: its repository as PhpStorm's project view shows it, read through the server's GitHub token
// (`repo_tree`, `repo_file`), so no checkout or token of the user's own is needed. The tree is on the left, folders
// first, at the branch the header's picker names (the default one until another is picked). Files open as tabs on the
// right, each read at the commit the tree was read at, so what opens matches the tree shown while the branch moves on;
// their lines are numbered and coloured by language, and select and copy as text. A binary file, or one over 1 MB,
// shows its size and a link to GitHub.
//
// Over the tree, PhpStorm's four finders: Go to Class (Ctrl+N), Go to File (Ctrl+Shift+N), Go to Symbol
// (Ctrl+Shift+Alt+N) and Find in Files (Ctrl+Shift+F). Files need only the tree; the other three search an index of
// the repository's source kept on this PC, in the encrypted response cache: every indexable file (repo_indexable) read
// through the API at the tree's commit, a few at a time. When the branch moves on, the index walks back through the
// new commits (`GET /commits/{sha}`) and reads again only the files they changed; one too far behind is read anew.
#include "repo.h"
#include "screens.h"
#include "sftp.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_ROW, A_RESULT, A_TAB, A_TAB_CLOSE, A_BRANCH, A_OPEN_GITHUB, A_COPY, A_RETRY, A_FOCUS_FIND, A_CLEAR_FIND, A_RETRY_FILE, A_MODE, A_INDEX_RETRY };
enum { ID_FIND = 0x5F30 };          // the finder's edit control id
enum { MAX_TABS = 20, MAX_RESULTS = 50, MAX_HITS = 200, GUTTER_BLOCK = 64 };
/// The finders, in the order their picker shows them.
enum { FIND_CLASSES, FIND_FILES, FIND_SYMBOLS, FIND_TEXT, FIND_MODES };
static const char *const FIND_TITLES[FIND_MODES] = { "Classes", "Files", "Symbols", "Text" };
static const wchar_t *const FIND_CUES[FIND_MODES] = { L"Go to class (Ctrl+N)", L"Go to file (Ctrl+Shift+N)", L"Go to symbol (Ctrl+Shift+Alt+N)", L"Find in files (Ctrl+Shift+F)" };
/// Files read at once while indexing; commits walked back before reading the index anew; symbols worked out again at
/// most this often (ms) while a search waits on an index being filled.
enum { FETCH_PARALLEL = 4, WALK_MAX = 50, SYMBOLS_EVERY_MS = 3000 };

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
    // The finder: its edit, where the last layout put it, and what it finds: tree entries (Files), the index's symbols
    // (Classes, Symbols) or lines (Text, `hits`).
    HWND find; RECT find_rc; bool find_laid, clipped;
    int mode;
    char *query; size_t *results; RepoTextHit *hits; size_t result_count, text_total; int pick;
    // The index: read from disk once per branch, then brought to the tree's commit (`target`) by walking back through
    // the commits since its own (`walk_at`, gathering `changed`) and reading the files in `queue`.
    RepoIndex index; bool index_read, index_dirty, symbols_stale; ULONGLONG symbols_at;
    struct IndexLoad *loading;
    char *target, *walk_at; char **changed; size_t changed_count; int walk_steps; Request *req_walk;
    char **queue; size_t queue_count, queue_next, fetched; Request *fetch[FETCH_PARALLEL];
    char *index_error;
    // A line to show once its file is laid out: a symbol's or a text hit's.
    char *goto_path; int goto_line, goto_y; bool goto_scroll; UINT timer;
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
    json_set_num(saved, "mode", p->mode);
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
static void index_sync(ProjectFiles *p);
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
    index_sync(p);
    relayout(p);
}

// MARK: - The index

static char *index_key(const ProjectFiles *p) { return xstrfmt("repo-index:%s@%s", p->repo, p->ref ? p->ref : ""); }
/// The commit the index's files are at, for walking back to: the one it was finished at, else the one a read cut short
/// was at (the files it lacks are read anyway).
static const char *index_base(const ProjectFiles *p) { return p->index.sha ? p->index.sha : p->index.partial; }

/// Reading the index from disk, off the UI thread; `cancelled` when its tab went first.
typedef struct IndexLoad { ProjectFiles *p; char *key; RepoIndex index; bool found, cancelled; } IndexLoad;
static void load_work(void *ctx) {
    IndexLoad *l = ctx;
    char *data = NULL; size_t len = 0;
    if (cache_bytes(g_store.cache, l->key, &data, &len)) {
        l->found = repo_index_parse(data, len, &l->index);
        if (l->found) repo_index_symbols(&l->index);
        // An index at the branch's commit is not written again, and pruning goes by the last write.
        if (l->found) cache_touch(g_store.cache, l->key);
    }
    free(data);
}
static void load_done(void *ctx) {
    IndexLoad *l = ctx;
    ProjectFiles *p = l->p;
    if (l->cancelled) { repo_index_free(&l->index); free(l->key); free(l); return; }
    p->loading = NULL;
    repo_index_free(&p->index);
    if (l->found) p->index = l->index; else repo_index_init(&p->index);
    set_string(&p->index.ref, p->ref);
    p->index_read = true; p->symbols_stale = false;
    free(l->key); free(l);
    find_update(p);
    index_sync(p);
    relayout(p);
}

/// Writing it, off the UI thread too; the bytes are the index as it was when asked.
typedef struct { char *key, *bytes; size_t len; } IndexSave;
static void save_work(void *ctx) { IndexSave *s = ctx; cache_store_bytes(g_store.cache, s->bytes, s->len, s->key); }
static void save_done(void *ctx) { IndexSave *s = ctx; free(s->key); free(s->bytes); free(s); }
static void index_save(ProjectFiles *p) {
    // Signed out, or refused (a 401): the cache was just cleared, and the repository's source is not written back.
    if (!p->index_read || !p->index_dirty || !store_connected()) return;
    // Mid-read, it is saved as partial: the next read takes it up where this one stopped.
    if (p->target && p->queue) { set_string(&p->index.sha, NULL); set_string(&p->index.partial, p->target); }
    IndexSave *s = xcalloc(1, sizeof *s);
    s->key = index_key(p);
    s->bytes = repo_index_serialize(&p->index, &s->len);
    p->index_dirty = false;
    async_run_cache(save_work, save_done, s);
}

static void symbols_refresh(ProjectFiles *p) {
    repo_index_symbols(&p->index);
    p->symbols_stale = false; p->symbols_at = GetTickCount64();
}
static void sync_stop(ProjectFiles *p) {
    request_cancel(&p->req_walk);
    for (int i = 0; i < FETCH_PARALLEL; i++) request_cancel(&p->fetch[i]);
    str_array_free(p->queue, p->queue_count); p->queue = NULL; p->queue_count = p->queue_next = p->fetched = 0;
    str_array_free(p->changed, p->changed_count); p->changed = NULL; p->changed_count = 0;
    set_string(&p->target, NULL); set_string(&p->walk_at, NULL);
}
static void sync_finish(ProjectFiles *p) {
    set_string(&p->index.sha, p->target); set_string(&p->index.partial, NULL);
    p->index_dirty = true;
    sync_stop(p);
    symbols_refresh(p);
    index_save(p);
    find_update(p);
    relayout(p);
}
static void sync_fail(ProjectFiles *p, const Request *req) {
    request_error_into(&p->index_error, req);
    index_save(p);
    sync_stop(p);
    if (p->symbols_stale) symbols_refresh(p);
    find_update(p);
    relayout(p);
}

static void fetch_fill(ProjectFiles *p);
static void fetch_done(void *owner, Request *req) {
    ProjectFiles *p = owner;
    // A file gone since the tree was read, or refused as a folder, is left out; anything else stops the read.
    bool skipped = !req->ok && req->error.kind == API_HTTP && (req->error.status == 404 || req->error.status == 400);
    if (!req->ok && !skipped) { sync_fail(p, req); return; }
    RepoFile file;
    if (req->ok && repo_file_parse(req->result, &file)) {
        // One the tree gave no size for is held to the index's limit here.
        size_t len = file.content ? strlen(file.content) : 0;
        if (file.content && p->index.bytes <= REPO_INDEX_MAX_BYTES && len <= REPO_INDEX_MAX_BYTES - p->index.bytes) { repo_index_put(&p->index, file.path, file.size, file.content, len); p->symbols_stale = true; }
        repo_file_free(&file);
    }
    p->fetched++; p->index_dirty = true;
    fetch_fill(p);
    // The progress line, every so often rather than for each file.
    if (p->target && p->fetched % 25 == 0) relayout(p);
}
static void fetch_fill(ProjectFiles *p) {
    bool busy = false;
    for (int i = 0; i < FETCH_PARALLEL; i++) {
        if (!p->fetch[i] && p->queue_next < p->queue_count) {
            Json *args = json_object();
            json_set_str(args, "repo", p->repo);
            json_set_str(args, "path", p->queue[p->queue_next++]);
            json_set_str(args, "ref", p->target);
            store_call("repo_file", args, 0, p, fetch_done, i, &p->fetch[i]);
        }
        if (p->fetch[i]) busy = true;
    }
    if (!busy) sync_finish(p);
}
/// Reads the files the index lacks at the target commit, after dropping the `changed` ones and those the tree has not.
static void fetch_start(ProjectFiles *p) {
    char **fetch = NULL;
    size_t n = repo_index_reconcile(&p->index, &p->tree, p->changed, p->changed_count, &fetch);
    str_array_free(p->changed, p->changed_count); p->changed = NULL; p->changed_count = 0;
    // Dropping files renumbers those after them, and the results point at them.
    symbols_refresh(p);
    find_update(p);
    p->index_dirty = true;
    p->queue = fetch; p->queue_count = n; p->queue_next = p->fetched = 0;
    fetch_fill(p);
    relayout(p);
}
/// Starts the index over: every indexable file is read at the target commit.
static void index_rebuild(ProjectFiles *p) {
    repo_index_free(&p->index); repo_index_init(&p->index);
    set_string(&p->index.ref, p->ref); set_string(&p->index.partial, p->target);
    str_array_free(p->changed, p->changed_count); p->changed = NULL; p->changed_count = 0;
    fetch_start(p);
}

static void walk_next(ProjectFiles *p);
static void walk_done(void *owner, Request *req) {
    ProjectFiles *p = owner;
    char **paths = NULL, *parent = NULL; size_t n = 0; bool truncated = false;
    if (!req->ok) {
        // A commit GitHub no longer has (a force push) leaves nothing to walk back through.
        if (req->error.kind == API_HTTP && (req->error.status == 404 || req->error.status == 422)) index_rebuild(p);
        else sync_fail(p, req);
        return;
    }
    if (!repo_commit_changes(req->result, &paths, &n, &parent, &truncated)) { index_rebuild(p); return; }
    p->changed = xrealloc(p->changed, (p->changed_count + n + 1) * sizeof *p->changed);
    memcpy(p->changed + p->changed_count, paths, n * sizeof *paths);
    p->changed_count += n;
    free(paths);
    p->walk_steps++;
    // A merge's files are against its first parent, so walking first parents back sees every change on the branch.
    if (parent && str_eq(parent, index_base(p))) fetch_start(p);
    else if (truncated || !parent || p->walk_steps >= WALK_MAX) index_rebuild(p);
    else { set_string(&p->walk_at, parent); walk_next(p); }
    free(parent);
}
static void walk_next(ProjectFiles *p) {
    Json *args = json_object();
    json_set_str(args, "repo", p->repo);
    json_set_str(args, "sha", p->walk_at);
    store_call("commit", args, 0, p, walk_done, 0, &p->req_walk);
}

/// Brings the index to the tree's commit, reading it from disk first.
static void index_sync(ProjectFiles *p) {
    if (!p->has_tree || !p->tree.sha || !project_files_offered()) return;
    if (!p->index_read) {
        if (!p->loading) {
            IndexLoad *l = xcalloc(1, sizeof *l);
            l->p = p; l->key = index_key(p);
            p->loading = l;
            async_run_cache(load_work, load_done, l);
        }
        return;
    }
    if (p->target) { if (str_eq(p->target, p->tree.sha)) return; index_save(p); sync_stop(p); }
    if (str_eq(p->index.sha, p->tree.sha)) return;
    set_string(&p->index_error, NULL);
    p->target = xstrdup(p->tree.sha);
    if (!p->index.sha && str_eq(p->index.partial, p->target)) fetch_start(p);
    else if (index_base(p) && store_supports("commit")) { p->walk_steps = 0; set_string(&p->walk_at, p->target); walk_next(p); }
    else index_rebuild(p);
    relayout(p);
}
/// Lets the index go: another branch has one of its own.
static void index_drop(ProjectFiles *p) {
    index_save(p);
    sync_stop(p);
    if (p->loading) { p->loading->cancelled = true; p->loading = NULL; }
    repo_index_free(&p->index); repo_index_init(&p->index);
    p->index_read = false; p->symbols_stale = false;
    set_string(&p->index_error, NULL);
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

// MARK: - Finding

static void find_update(ProjectFiles *p) {
    free(p->results); p->results = NULL; free(p->hits); p->hits = NULL; p->result_count = p->text_total = 0;
    if (str_empty(p->query)) { p->pick = 0; return; }
    if (p->mode == FIND_FILES) { if (p->has_tree) p->results = repo_find_files(&p->tree, p->query, MAX_RESULTS, &p->result_count); }
    else if (p->mode == FIND_TEXT) { if (strlen(p->query) >= 2) p->hits = repo_index_find_text(&p->index, p->query, MAX_HITS, &p->result_count, &p->text_total); }
    else {
        // While files come in, the declarations are worked out again now and then, not for each one.
        if (p->symbols_stale && GetTickCount64() - p->symbols_at >= SYMBOLS_EVERY_MS) symbols_refresh(p);
        p->results = repo_index_find_symbols(&p->index, p->query, p->mode == FIND_CLASSES, MAX_RESULTS, &p->result_count);
    }
    if (p->pick >= (int)p->result_count) p->pick = p->result_count ? (int)p->result_count - 1 : 0;
    if (p->pick < 0) p->pick = 0;
}
static void find_clear(ProjectFiles *p) {
    if (p->find) SetWindowTextW(p->find, L"");   // its EN_CHANGE clears the query
    set_string(&p->query, NULL); find_update(p);
}
/// Opens a file at a line, which shows lit once the file is laid out; 0 for its top.
static void open_at(ProjectFiles *p, const char *path, int line) {
    set_string(&p->goto_path, line > 0 ? path : NULL);
    p->goto_line = line; p->goto_scroll = line > 0; p->goto_y = -1;
    open_path(p, path);
}
static void open_result(ProjectFiles *p, size_t k) {
    if (k >= p->result_count) return;
    char *path; int line = 0;
    if (p->mode == FIND_FILES) path = xstrdup(p->tree.entries[p->results[k]].path);
    else if (p->mode == FIND_TEXT) { path = xstrdup(p->index.files[p->hits[k].file].path); line = p->hits[k].line; }
    else { const RepoSymbol *sym = &p->index.symbols[p->results[k]]; path = xstrdup(p->index.files[sym->file].path); line = sym->line; }
    // The query stays for Find in Files, so the next hit is a click away; the Go to finders close as PhpStorm's do.
    if (p->mode != FIND_TEXT) find_clear(p);
    if (p->host->pane) SetFocus(pane_hwnd(p->host->pane));
    open_at(p, path, line);
    free(path);
}
static void set_mode(ProjectFiles *p, int mode) {
    if (mode < 0 || mode >= FIND_MODES) return;
    p->mode = mode; p->pick = 0;
    if (p->find) SendMessageW(p->find, EM_SETCUEBANNER, TRUE, (LPARAM)FIND_CUES[mode]);
    find_update(p);
    state_save(p);
    relayout(p);
}
/// PhpStorm's keys: Ctrl+N a class, Ctrl+Shift+N a file (Ctrl+P too), Ctrl+Shift+Alt+N a symbol, Ctrl+Shift+F text.
static int mode_for_key(WPARAM vk, bool ctrl, bool shift) {
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
        int mode = mode_for_key(wp, ctrl, shift);
        if (mode >= 0) { set_mode(p, mode); SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_DOWN || wp == VK_UP) {
            int n = (int)p->result_count;
            if (n) { p->pick = (p->pick + (wp == VK_DOWN ? 1 : n - 1)) % n; relayout(p); }
            return 0;
        }
        if (wp == VK_RETURN) { open_result(p, (size_t)p->pick); return 0; }
        if (wp == VK_ESCAPE) { find_clear(p); SetFocus(GetParent(hwnd)); relayout(p); return 0; }
        if (wp == 'A' && ctrl) { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        break;
    }
    case WM_CHAR:
        // Enter, Escape and the control characters the shortcuts above leave behind.
        if (wp == '\r' || wp == 0x1B || (ctrl && wp < 0x20)) return 0;
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
    int mode = mode_for_key(vk, ctrl, shift);
    if (mode < 0 || !p->find) return false;
    set_mode(p, mode);
    if (p->host->pane) pane_scroll_to_top(p->host->pane);
    SetFocus(p->find);
    SendMessageW(p->find, EM_SETSEL, 0, -1);
    return true;
}
bool project_files_timer(ProjectFiles *p, UINT id) {
    if (id != p->timer || !p->host->pane) return false;
    KillTimer(pane_hwnd(p->host->pane), id);
    pane_scroll_to(p->host->pane, p->goto_y);
    return true;
}
void project_files_place(ProjectFiles *p, const RECT *content, int scroll_y, bool shown) {
    // The line a finder opened, once its file is laid out: scrolled to after this layout, not inside it.
    if (shown && p->goto_scroll && p->goto_y >= 0 && p->host->pane) { p->goto_scroll = false; SetTimer(pane_hwnd(p->host->pane), p->timer, 1, NULL); }
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

/// A finder's result: a file's name and folder, a declaration's name and where it is (a letter in its kind's colour
/// before it, as PhpStorm's C, m and K), or a line holding the text searched for and where it is.
typedef struct { char *name, *detail; char badge; COLORREF badge_color; bool picked, line; } ResultData;
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
static void paint_lit(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    fill_rect(cv, rc, blend(theme.accent, theme.raise, 0.18));
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

/// The text of a hit's line, its leading spaces dropped and cut at 200 bytes.
static char *hit_line(const RepoIndexFile *f, const RepoTextHit *h) {
    size_t n = f->len, start = h->start < n ? h->start : n;
    const char *s = f->content;
    while (start < n && (s[start] == ' ' || s[start] == '\t')) start++;
    size_t end = start;
    while (end < n && s[end] != '\n' && s[end] != '\r' && end - start < 200) end++;
    return xstrndup(s + start, end - start);
}
static void layout_results(ProjectFiles *p, Doc *doc, int x, int w) {
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
static void layout_modes(ProjectFiles *p, Doc *doc, int x, int w) {
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
    else if (p->target && p->req_walk) status = xstrfmt("Looking for what changed since %.7s\xE2\x80\xA6", index_base(p));
    else if (p->target) status = xstrfmt("Indexing %zu of %zu files\xE2\x80\xA6", p->fetched, p->queue_count);
    else if (p->mode != FIND_FILES && p->index_read && p->index.sha)
        status = xstrfmt("%zu files and %zu declarations indexed at %.7s", p->index.count, p->index.symbol_count, p->index.sha);
    if (status) { doc_space(doc, px(6)); doc_text(doc, x + px(4), w - px(8), status, FONT_CAPTION2, p->target ? theme.secondary : theme.tertiary, DT_LEFT | DT_WORDBREAK); free(status); }
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
            doc_add(doc, &lr, paint_lit);
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
    // The finder, and the branch being read when it changes.
    layout_find(p, doc, 0, tree_w);
    if (p->req && p->loaded) {
        char *line = xstrfmt("Reading %s\xE2\x80\xA6", shown_ref(p));
        RECT r = { wide ? tree_w + gap : 0, doc->y - px(34), w, doc->y };
        if (!wide) { doc_space(doc, px(4)); r.top = doc->y; r.bottom = doc->y + px(18); doc->y = r.bottom; }
        doc_text_at(doc, &r, line, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        free(line);
    }
    doc_space(doc, px(8));
    layout_modes(p, doc, 0, tree_w);
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
    index_drop(p);
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
        else open_at(p, p->tree.entries[arg].path, 0);
        break;
    case A_RESULT: open_result(p, (size_t)arg); break;
    case A_TAB: if ((size_t)arg < p->tab_count) { set_string(&p->goto_path, NULL); activate(p, (int)arg); state_save(p); relayout(p); } break;
    case A_MODE: set_mode(p, (int)arg); if (p->find) SetFocus(p->find); break;
    case A_INDEX_RETRY: set_string(&p->index_error, NULL); index_sync(p); relayout(p); break;
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

ProjectFiles *project_files_new(const char *repo, Screen *host, int action_base, UINT timer) {
    ProjectFiles *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo); p->host = host; p->base = action_base; p->active = -1; p->timer = timer; p->goto_y = -1;
    p->mode = FIND_FILES;
    repo_index_init(&p->index);
    // The branch and tabs open last time; each file is read once the tree says at which commit.
    char *key = state_key(p);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    set_string(&p->ref, json_str_nonempty(json_get(saved, "ref")));
    const Json *tabs = json_get(saved, "tabs");
    for (size_t i = 0; i < json_count(tabs) && p->tab_count < (size_t)MAX_TABS; i++) { const char *path = json_str_nonempty(json_at(tabs, i)); if (path) tab_add(p, path); }
    int active = json_int_or(json_get(saved, "active"), 0);
    p->active = p->tab_count ? (active >= 0 && (size_t)active < p->tab_count ? active : 0) : -1;
    int mode = json_int_or(json_get(saved, "mode"), FIND_FILES);
    if (mode >= 0 && mode < FIND_MODES) p->mode = mode;
    json_free(saved);
    return p;
}
void project_files_free(ProjectFiles *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->req_branches);
    // What was read so far is kept for next time.
    index_drop(p);
    if (p->host->pane) KillTimer(pane_hwnd(p->host->pane), p->timer);
    for (size_t i = 0; i < p->tab_count; i++) file_free(p->tabs[i]);
    free(p->tabs);
    if (p->find) DestroyWindow(p->find);
    if (p->has_tree) repo_tree_free(&p->tree);
    str_array_free(p->branches, p->branch_count);
    free(p->repo); free(p->ref); free(p->error); free(p->default_branch); free(p->query); free(p->results); free(p->hits);
    free(p->goto_path);
    free(p);
}
