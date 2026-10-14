#include "screen_files_internal.h"

typedef struct { Screen base; int number; PullFiles *files; } FilesScreen;

static char *files_key(PullFiles *f);
static void list_changed(PullFiles *f);
static void files_done(void *owner, Request *req);
static void files_destroy(Screen *base);
static void files_layout(Screen *base, Doc *doc);
static void files_header(Screen *base, HeaderInfo *info);
static void files_action(Screen *base, int action, intptr_t arg, POINT pt);
static void files_timer(Screen *base, UINT id);
static void files_visible(Screen *base, bool shown);
static void files_refresh(Screen *base);

// MARK: - Reading

static char *files_key(PullFiles *f) { return xstrfmt("files:%s#%d", f->project.repo, f->number); }

Pane *screen_files_files_pane(PullFiles *f) { return f->host->pane; }

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
        pane_relayout(screen_files_files_pane(f));
        return;
    }
    PullFilesPage page;
    if (!pull_files_page_parse(req->result, &page)) { set_string(&f->error, "The server returned an unexpected response."); pane_relayout(screen_files_files_pane(f)); return; }
    set_string(&f->error, NULL);
    f->retried = false;
    if (!f->confirmed) {
        f->confirmed = true;
        // The same revision has the same files; only a push or rebase makes them worth reading again.
        if (pull_file_list_confirm(&f->list, &page)) { pull_files_page_free(&page); pane_relayout(screen_files_files_pane(f)); return; }
        pull_file_list_free(&f->list); pull_file_list_init(&f->list); list_changed(f);
    }
    pull_file_list_append(&f->list, &page); list_changed(f);
    pull_files_page_free(&page);
    // Only whole lists are saved, so a saved one never waits on a page.
    if (!f->list.next_page) { char *key = files_key(f); Json *j = pull_file_list_json(&f->list); cache_store(g_store.cache, j, key); json_free(j); free(key); }
    pane_relayout(screen_files_files_pane(f));
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
    KillTimer(pane_hwnd(screen_files_files_pane(f)), id);
    pull_files_load(f);
    return true;
}

bool pull_files_action(PullFiles *f, int action, intptr_t arg) {
    int a = action - f->action_base;
    if (a < 0 || a >= PULL_FILES_ACTIONS) return false;
    switch (a) {
    case ACT_SELECT:
        if ((size_t)arg < f->list.file_count) { f->selected = (int)arg; set_string(&f->selected_path, f->list.files[arg].filename); pane_relayout(screen_files_files_pane(f)); }
        break;
    case ACT_TOGGLE: if ((size_t)arg < f->node_count) { screen_files_dir_toggle(f, f->nodes[arg].path); pane_relayout(screen_files_files_pane(f)); } break;
    case ACT_RETRY: pull_files_load(f); pane_relayout(screen_files_files_pane(f)); break;
    case ACT_WRAP: f->wrap = !f->wrap; pane_relayout(screen_files_files_pane(f)); break;
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
    screen_files_tree_free(f);
    for (size_t i = 0; i < f->collapsed_count; i++) free(f->collapsed[i]);
    free(f->collapsed);
    free(f->selected_path);
    diff_free(f->lines, f->line_count);
    free(f);
}

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
