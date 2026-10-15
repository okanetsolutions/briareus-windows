#include "project_files_internal.h"

/// Writing it, off the UI thread too; the bytes are the index as it was when asked.
typedef struct { char *key, *bytes; size_t len; } IndexSave;

static char *index_key(const ProjectFiles *p);
static void load_work(void *ctx);
static void load_done(void *ctx);
static void save_work(void *ctx);
static void save_done(void *ctx);
static void index_save(ProjectFiles *p);
static void sync_stop(ProjectFiles *p);
static void sync_finish(ProjectFiles *p);
static void sync_fail(ProjectFiles *p, const Request *req);
static void fetch_done(void *owner, Request *req);
static void fetch_fill(ProjectFiles *p);
static void fetch_start(ProjectFiles *p);
static void index_rebuild(ProjectFiles *p);
static void walk_done(void *owner, Request *req);
static void walk_next(ProjectFiles *p);

// MARK: - The index

static char *index_key(const ProjectFiles *p) { return xstrfmt("repo-index:%s@%s", p->repo, p->ref ? p->ref : ""); }

/// The commit the index's files are at, for walking back to: the one it was finished at, else the one a read cut short
/// was at (the files it lacks are read anyway).
const char *project_files_index_base(const ProjectFiles *p) { return p->index.sha ? p->index.sha : p->index.partial; }

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
    project_files_find_update(p);
    project_files_index_sync(p);
    project_files_relayout(p);
}

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

void project_files_symbols_refresh(ProjectFiles *p) {
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
    project_files_symbols_refresh(p);
    index_save(p);
    project_files_find_update(p);
    project_files_relayout(p);
}

static void sync_fail(ProjectFiles *p, const Request *req) {
    request_error_into(&p->index_error, req);
    index_save(p);
    sync_stop(p);
    if (p->symbols_stale) project_files_symbols_refresh(p);
    project_files_find_update(p);
    project_files_relayout(p);
}

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
    if (p->target && p->fetched % 25 == 0) project_files_relayout(p);
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
    project_files_symbols_refresh(p);
    project_files_find_update(p);
    p->index_dirty = true;
    p->queue = fetch; p->queue_count = n; p->queue_next = p->fetched = 0;
    fetch_fill(p);
    project_files_relayout(p);
}

/// Starts the index over: every indexable file is read at the target commit.
static void index_rebuild(ProjectFiles *p) {
    repo_index_free(&p->index); repo_index_init(&p->index);
    set_string(&p->index.ref, p->ref); set_string(&p->index.partial, p->target);
    str_array_free(p->changed, p->changed_count); p->changed = NULL; p->changed_count = 0;
    fetch_start(p);
}

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
    // A truncated commit's list misses files, so even one right after the base needs the whole tree read again.
    if (truncated || !parent) index_rebuild(p);
    else if (str_eq(parent, project_files_index_base(p))) fetch_start(p);
    else if (p->walk_steps >= WALK_MAX) index_rebuild(p);
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
void project_files_index_sync(ProjectFiles *p) {
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
    else if (project_files_index_base(p) && store_supports("commit")) { p->walk_steps = 0; set_string(&p->walk_at, p->target); walk_next(p); }
    else index_rebuild(p);
    project_files_relayout(p);
}

/// Lets the index go: another branch has one of its own.
void project_files_index_drop(ProjectFiles *p) {
    index_save(p);
    sync_stop(p);
    if (p->loading) { p->loading->cancelled = true; p->loading = NULL; }
    repo_index_free(&p->index); repo_index_init(&p->index);
    p->index_read = false; p->symbols_stale = false;
    set_string(&p->index_error, NULL);
}
