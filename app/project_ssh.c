// A project's SSH sessions tab, beside its pull requests and issues: the SSH servers registered for the project in Settings
// down the left, and its open sessions as tabs over a terminal on the right. A click on a server opens an SSH session to it
// (or returns to the open one). Sessions run this PC's OpenSSH client
// (terminal.c) straight to the server, so its keys, agent and ~/.ssh/config apply, and they stay open while other screens
// are shown.
#include "project_ssh_internal.h"
#include "str.h"
#include "ssh_settings.h"
#include "terminal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ProjectSsh *g_instances[MAX_INSTANCES];

bool project_ssh_offered(void) { return store_supports("settings_ssh_servers"); }

int project_ssh_server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }

/// The project's session on show: the active one when it is the project's, else its first.
Term *project_ssh_shown_term(ProjectSsh *p) {
    Term *active = term_active();
    if (active && (p->global || str_eq(term_group(active), p->repo))) return active;
    for (size_t i = 0; i < term_count(); i++) if (p->global || str_eq(term_group(term_at(i)), p->repo)) return term_at(i);
    return NULL;
}
size_t project_ssh_session_count(const char *repo) {
    size_t n = 0;
    for (size_t i = 0; i < term_count(); i++) if (str_eq(term_group(term_at(i)), repo)) n++;
    return n;
}

static void relayout(ProjectSsh *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static void terms_changed(void *ctx) { relayout(ctx); }

// MARK: - Loading

static void rows_done(void *owner, Request *req) {
    ProjectSsh *p = owner;
    if (!project_ssh_current(p) || req->client != p->account) return;
    p->loaded = true; p->loading = false;
    if (!req->ok) request_error_into(&p->error, req);
    else if (!json_is_array(json_get(req->result, "servers"))) {
        char *error = request_error_or_unexpected(req); set_string(&p->error, error); free(error);
    }
    else {
        set_string(&p->error, NULL);
        json_free(p->rows); p->rows = json_array();
        const Json *all = json_get(req->result, "servers");
        for (size_t i = 0; i < json_count(all); i++)
            if (p->global || str_eq(json_str(json_get(json_at(all, i), "repo")), p->repo)) json_array_push(p->rows, json_clone(json_at(all, i)));
        for (size_t i = 0; i < json_count(p->rows); i++) ssh_secrets_scrub((Json *)json_at(p->rows, i));
    }
    relayout(p);
}
void project_ssh_load(ProjectSsh *p) {
    servers_ssh_scope_changed();
    if (!project_ssh_current(p) || p->loaded || p->req) return;
    if (!project_ssh_offered()) { p->loaded = true; return; }
    p->loading = true;
    store_call("settings_ssh_servers", json_object(), 0, p, rows_done, 0, &p->req);
}
void project_ssh_refresh(ProjectSsh *p) {
    request_cancel(&p->req);
    p->loaded = false;
    project_ssh_load(p);
}
void servers_ssh_scope_changed(void) {
    servers_ssh_sessions_check();
    for (int i = 0; i < MAX_INSTANCES; i++) {
        ProjectSsh *p = g_instances[i];
        if (!p || project_ssh_current(p)) continue;
        request_cancel(&p->req);
        json_free(p->rows); p->rows = json_array();
        json_free(p->folded); p->folded = json_object();
        set_string(&p->error, NULL);
        p->loaded = p->loading = false; p->focused = NULL;
        api_client_release(p->account); p->account = g_store.client; api_client_retain(p->account);
        free(p->scope); p->scope = project_ssh_scope();
        relayout(p);
    }
}
void servers_ssh_changed(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] && (g_instances[i]->loaded || g_instances[i]->req)) project_ssh_refresh(g_instances[i]);
    servers_sftp_changed();
}

// MARK: - Connecting

void project_ssh_connect_row(ProjectSsh *p, const Json *row) {
    if (!project_ssh_current(p) || !project_ssh_offered()) return;
    if (!json_is_object(row) || !p->host->pane) return;
    char *key = ssh_server_key(row), *name = ssh_server_name(row);
    Term *open = term_find(key);
    if (open) term_set_active(open);
    else {
        servers_ssh_sessions_bind();
        TermTarget target = { key, p->global ? json_str(json_get(row, "repo")) : p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), project_ssh_server_port(row) };
        char *error = NULL;
        if (!term_open(pane_hwnd(p->host->pane), &target, &error)) {
            char *title = xstrfmt("Could not connect to %s", name);
            app_alert(title, error ? error : "The session could not start.");
            free(title);
        }
        free(error);
    }
    free(key); free(name);
    relayout(p);
}
static void close_term(Term *t) {
    if (!t) return;
    if (term_running(t)) {
        char *title = xstrfmt("Disconnect from %s?", term_label(t));
        bool ok = app_confirm(title, "The session ends and anything running in it on the server is stopped with it.", "Disconnect", true);
        free(title);
        if (!ok) return;
    }
    term_close(t);
}

void project_ssh_place(ProjectSsh *p, const RECT *content, int scroll_y, bool shown) {
    Term *active = shown ? project_ssh_shown_term(p) : NULL;
    RECT r = { 0 };
    bool on = active && content && !IsRectEmpty(&p->term_rc);
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(p->host->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(p->host->pane)) / 2;
        SetRect(&r, content->left + m + p->term_rc.left, content->top + p->term_rc.top - scroll_y, content->left + m + p->term_rc.right, content->top + p->term_rc.bottom - scroll_y);
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else r = visible;
    }
    // Only this project's session on show is up; every other one waits, hidden.
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        if (t == active && on) {
            SetParent(term_hwnd(t), pane_hwnd(p->host->pane));
            term_cycle_all(t, p->global); term_place(t, &r, true);
        }
        else if (p->global || str_eq(term_group(t), p->repo) || !shown) term_place(t, NULL, false);
    }
    if (!on) { p->focused = NULL; return; }
    // A session just opened or switched to takes the keyboard.
    if (p->focused != active) { p->focused = active; term_focus(active); }
}

// MARK: - Actions

static Term *term_by_index(intptr_t arg) { return arg >= 0 ? term_at((size_t)arg) : NULL; }
bool project_ssh_action(ProjectSsh *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    servers_ssh_scope_changed();
    if (!project_ssh_current(p) || !project_ssh_offered()) return false;
    if (action < p->base || action >= p->base + PROJECT_SSH_ACTIONS) return false;
    switch (action - p->base) {
    case A_FOLDER: {
        size_t count; const Project *projects = projects_list(&count);
        Json *folders = ssh_server_folders(p->rows, projects, count, "");
        const char *repo = json_str(json_get(json_at(folders, (size_t)arg), "repo"));
        if (repo) ssh_server_folder_toggle(p->folded, repo, "");
        json_free(folders); relayout(p); return true;
    }
    case A_SERVER: project_ssh_connect_row(p, json_at(p->rows, (size_t)arg)); break;
    case A_TAB: term_set_active(term_by_index(arg)); break;
    case A_TAB_CLOSE: close_term(term_by_index(arg)); break;
    case A_CLOSE: close_term(project_ssh_shown_term(p)); break;
    case A_RECONNECT: {
        Term *t = project_ssh_shown_term(p);
        char *error = NULL;
        if (t && !term_reconnect(t, &error)) app_alert("Could not reconnect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    }
    // The click gave the pane the keyboard; the session on show takes it back even when nothing changed, as on its own
    // tab or server or a cancelled Close.
    p->focused = NULL;
    relayout(p);
    return true;
}

// MARK: - Lifetime

ProjectSsh *project_ssh_new(const char *repo, Screen *host, int action_base) {
    ProjectSsh *p = xcalloc(1, sizeof *p);
    servers_ssh_scope_changed();
    p->global = repo == NULL;
    p->repo = xstrdup(repo ? repo : "");
    p->account = g_store.client; api_client_retain(p->account);
    p->scope = project_ssh_scope();
    p->folded = json_object();
    p->host = host; p->base = action_base;
    p->rows = json_array();
    for (int i = 0; i < MAX_INSTANCES; i++) if (!g_instances[i]) { g_instances[i] = p; break; }
    term_add_listener(terms_changed, p);
    return p;
}
void project_ssh_free(ProjectSsh *p) {
    if (!p) return;
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] == p) g_instances[i] = NULL;
    term_remove_listener(terms_changed, p);
    // The sessions go on; their windows wait, hidden, for the tab to come back.
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        if (p->host->pane && GetParent(term_hwnd(t)) == pane_hwnd(p->host->pane)) term_place(t, NULL, false);
    }
    request_cancel(&p->req);
    json_free(p->rows);
    api_client_release(p->account); free(p->scope);
    json_free(p->folded);
    free(p->repo); free(p->error);
    free(p);
}
