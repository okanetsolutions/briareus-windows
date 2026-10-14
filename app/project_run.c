#include "project_run_internal.h"

static void show_url(ProjectRun *p, const char *url);
static void branch_from(ProjectRun *p, const Json *session);
static void follow(ProjectRun *p, const char *session);
static void log_events_done(void *owner, Request *req);
static void log_find_done(void *owner, Request *req);
static void log_tick(ProjectRun *p);
static void run_done(void *owner, Request *req);
static void start(ProjectRun *p);
static void resume(ProjectRun *p);
static void sessions_done(void *owner, Request *req);
static void profiles_done(void *owner, Request *req);
static void pick_profile(ProjectRun *p, POINT pt);
static void forget(ProjectRun *p);
static void delete_done(void *owner, Request *req);
static void delete_run(ProjectRun *p);

bool project_run_offered(void) { return store_can_manage() && store_supports("serve_branch"); }

void project_run_changed(ProjectRun *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

void project_run_web_changed(void *ctx) {
    ProjectRun *p = ctx;
    if (p->host->pane && pane_top(p->host->pane) == p->host) project_run_changed(p);
}

void project_run_timer_on(ProjectRun *p, bool on) {
    if (!p->host->pane) return;
    if (on) SetTimer(pane_hwnd(p->host->pane), p->timer, 1500, NULL);
    else KillTimer(pane_hwnd(p->host->pane), p->timer);
}

/// The session the tab shows: the one serving it, else the one being prepared for it.
const char *project_run_target(ProjectRun *p) { return p->session ? p->session : p->log_session; }

/// The profile shown as chosen: the one picked, else the one served, else the project's default.
const char *project_run_shown_profile(ProjectRun *p) {
    if (p->want) return p->want;
    if (p->profile) return p->profile;
    return p->profile_count ? p->profiles[0] : NULL;
}

/// Shows the served address, starting the browser again when the address changed.
static void show_url(ProjectRun *p, const char *url) {
    if (!str_eq(p->url, url)) { webview_free(p->web); p->web = NULL; set_string(&p->url, url); }
    project_run_changed(p);
}

static void branch_from(ProjectRun *p, const Json *session) {
    const char *b = json_str_nonempty(json_get(session, "startBranch"));
    if (!b) b = json_str_nonempty(json_get(session, "branch"));
    if (b) set_string(&p->branch, b);
}

// MARK: - The log

static void follow(ProjectRun *p, const char *session) {
    if (str_eq(p->log_session, session)) return;
    set_string(&p->log_session, session);
    run_log_clear(&p->log);
}

static void log_events_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    if (!req->ok || !str_eq(json_str(json_get(req->args, "sessionId")), p->log_session)) return;
    if (run_log_add_events(&p->log, json_get(req->result, "events")) && p->host->pane) pane_relayout(p->host->pane);
}

/// While `serve_branch` prepares the session, its id is not known yet: the newest branch preview of the project is it.
static void log_find_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    if (!req->ok || p->session) return;
    char *id = run_session_preparing_branch(req->result);
    if (id) follow(p, id);
    free(id);
}

static void log_tick(ProjectRun *p) {
    if (!p->busy) { project_run_timer_on(p, false); return; }
    if (p->req_log) return;
    if (p->session) follow(p, p->session);
    if (p->log_session && store_supports("session")) {
        Json *a = json_object(); json_set_str(a, "sessionId", p->log_session); json_set_num(a, "since", p->log.cursor);
        store_call("session", a, 0, p, log_events_done, 0, &p->req_log);
    } else if (!p->session && store_supports("sessions")) {
        Json *a = json_object(); json_set_str(a, "repo", p->repo);
        store_call("sessions", a, 0, p, log_find_done, 0, &p->req_log);
    }
}

static void run_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    p->busy = false;
    bool switched = json_str(json_get(req->args, "sessionId")) != NULL;
    char *asked = p->asked; p->asked = NULL;
    if (!req->ok) {
        request_error_into(&p->serve_error, req);
        if (switched && (req->error.status == 404 || (req->error.message && strstr(req->error.message, "no live workspace")))) {
            // The session closed or expired, and its page with it: the next try prepares a new one.
            set_string(&p->session, NULL); set_string(&p->url, NULL);
            webview_free(p->web); p->web = NULL;
        } else if (switched) {
            // A refused switch leaves the app serving what it served.
            set_string(&p->want, p->profile);
        }
    } else {
        const Json *session = json_get(req->result, "session");
        const char *id = json_str(json_get(session, "id"));
        if (id) set_string(&p->session, id);
        branch_from(p, session);
        set_string(&p->profile, json_str(json_get(req->result, "profile")));
        const char *url = json_str(json_get(req->result, "url"));
        if (safe_web_url(url)) {
            bool same = str_eq(url, p->url);
            show_url(p, url);
            // The same address after a restart is a new app behind it.
            if (same && p->web) webview_reload(p->web);
        } else set_string(&p->serve_error, "The server did not say where it serves the branch.");
        // A profile picked while this request was out, or a new session served with the default.
        if (p->want && p->session && !str_eq(p->want, asked) && !str_eq(p->want, p->profile)) { free(asked); start(p); return; }
    }
    free(asked);
    project_run_changed(p);
}

/// Serves the branch: the run's session again, with the picked profile, when there is one; else a new session, which
/// the server serves with the project's default profile (switched afterwards when another was picked).
static void start(ProjectRun *p) {
    if (p->busy) return;
    p->busy = true; set_string(&p->serve_error, NULL);
    Json *args = json_object();
    const char *op = "serve_branch";
    if (p->session && store_supports("serve")) {
        op = "serve";
        json_set_str(args, "sessionId", p->session);
        if (p->want) json_set_str(args, "profile", p->want);
        set_string(&p->asked, p->want);
    } else json_set_str(args, "repo", p->repo);
    store_call(op, args, 170000, p, run_done, 0, &p->req_run);
    if (p->asked) { char *t = xstrfmt("\xE2\x96\xB6 Switching to profile %s", p->asked); run_log_add(&p->log, t, false); free(t); }
    project_run_timer_on(p, true);
    project_run_changed(p);
}

static void resume(ProjectRun *p) { if (!p->url && !p->busy && !p->serve_error) start(p); }

/// A Run already serving the branch (a branch preview with a serve link) is shown as it is.
static void sessions_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    p->sessions_read = true;
    if (req->ok && !p->url && !p->busy) {
        char *id = NULL, *url = NULL;
        if (run_session_serving_branch(req->result, &id, &url)) {
            set_string(&p->session, id);
            const Json *rows = json_get(req->result, "sessions");
            for (size_t i = 0; i < json_count(rows); i++) if (str_eq(json_str(json_get(json_at(rows, i), "id")), id)) branch_from(p, json_at(rows, i));
            show_url(p, url);
        }
        free(id); free(url);
    }
    if (p->pending) { p->pending = false; resume(p); }
    project_run_changed(p);
}

static void profiles_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    if (!req->ok) return;
    p->profiles_read = true;
    str_array_free(p->profiles, p->profile_count);
    p->profiles = run_profiles_parse(req->result, p->repo, &p->profile_count);
    project_run_changed(p);
}

void project_run_open(ProjectRun *p) {
    if (!p->profiles_read && !p->req_profiles && store_supports("projects")) store_call("projects", json_object(), 0, p, profiles_done, 0, &p->req_profiles);
    // Until the project's sessions are read, a Run serving the branch may be among them.
    if (!p->sessions_read && store_supports("sessions")) {
        p->pending = true;
        if (!p->req_sessions) { Json *a = json_object(); json_set_str(a, "repo", p->repo); store_call("sessions", a, 0, p, sessions_done, 0, &p->req_sessions); }
    } else resume(p);
    if (p->busy) project_run_timer_on(p, true);
    project_run_changed(p);
}

static void pick_profile(ProjectRun *p, POINT pt) {
    if (!p->profile_count) return;
    HMENU menu = CreatePopupMenu();
    const char *shown = project_run_shown_profile(p);
    for (size_t i = 0; i < p->profile_count; i++) {
        char *label = i ? xstrdup(p->profiles[i]) : xstrfmt("%s (default)", p->profiles[i]);
        wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (str_eq(shown, p->profiles[i]) ? MF_CHECKED : 0), (UINT_PTR)(1 + i), wl);
        free(wl); free(label);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || (size_t)chosen > p->profile_count || str_eq(p->profiles[chosen - 1], shown)) return;
    set_string(&p->want, p->profiles[chosen - 1]);
    // While a request is out, its answer switches to the new pick.
    if (!p->busy) start(p);
    project_run_changed(p);
}

// MARK: - Deleting

/// The run's session was deleted, and the workspace serving it with it: opening the tab again prepares a new one.
static void forget(ProjectRun *p) {
    request_cancel(&p->req_run); request_cancel(&p->req_log);
    p->busy = false;
    set_string(&p->session, NULL); set_string(&p->url, NULL); set_string(&p->profile, NULL);
    set_string(&p->asked, NULL); set_string(&p->serve_error, NULL);
    set_string(&p->log_session, NULL); run_log_clear(&p->log);
    webview_free(p->web); p->web = NULL;
    project_run_timer_on(p, false);
}

static void delete_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    p->deleting = false;
    const char *id = json_str(json_get(req->args, "sessionId"));
    if (req->ok) {
        set_string(&p->delete_error, NULL);
        if (str_eq(id, p->session) || str_eq(id, p->log_session)) forget(p);
        // The sidebar drops the row now instead of at its next poll.
        sessions_forget(p->repo, id);
    } else request_error_into(&p->delete_error, req);
    project_run_changed(p);
}

static void delete_run(ProjectRun *p) {
    if (!project_run_target(p) || p->deleting || !store_supports("delete")) return;
    char *id = xstrdup(project_run_target(p));
    bool ok = app_confirm("Delete this run?", "Its workspace stops serving the branch, and its conversation and transcript are deleted permanently.", "Delete", true);
    // The run may have changed while the dialog was open: delete the one asked about only if it is still shown.
    if (ok && !p->deleting && str_eq(id, project_run_target(p))) {
        p->deleting = true; set_string(&p->delete_error, NULL);
        Json *args = json_object(); json_set_str(args, "sessionId", id);
        store_call("delete", args, 0, p, delete_done, 0, &p->req_delete);
        project_run_changed(p);
    }
    free(id);
}

void project_run_refresh(ProjectRun *p) {
    // F5 reloads the page, as in a browser; with none up, it tries again.
    if (p->web) { webview_reload(p->web); return; }
    if (!p->busy) { set_string(&p->serve_error, NULL); start(p); }
}

bool project_run_timer(ProjectRun *p, UINT id) {
    if (id != p->timer) return false;
    log_tick(p);
    return true;
}

bool project_run_action(ProjectRun *p, int action, intptr_t arg, POINT pt) {
    (void)arg;
    if (action < p->base || action >= p->base + PROJECT_RUN_ACTIONS) return false;
    switch (action - p->base) {
    case A_PROFILE: pick_profile(p, pt); break;
    case A_RELOAD: if (p->web) webview_reload(p->web); break;
    case A_BACK: if (p->web) webview_back(p->web); break;
    case A_FORWARD: if (p->web) webview_forward(p->web); break;
    case A_BROWSER: { const char *url = p->web && webview_url(p->web) ? webview_url(p->web) : p->url; if (url) open_web_url(url); break; }
    case A_RETRY: start(p); break;
    case A_DELETE: delete_run(p); break;
    }
    return true;
}

ProjectRun *project_run_new(const char *repo, Screen *host, int action_base, UINT timer) {
    ProjectRun *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo); p->host = host; p->base = action_base; p->timer = timer;
    return p;
}

void project_run_free(ProjectRun *p) {
    if (!p) return;
    request_cancel(&p->req_run); request_cancel(&p->req_profiles); request_cancel(&p->req_log);
    request_cancel(&p->req_sessions); request_cancel(&p->req_delete); request_cancel(&p->req_access);
    project_run_timer_on(p, false);
    run_address_free(&p->address);
    webview_free(p->web);
    run_log_clear(&p->log);
    str_array_free(p->profiles, p->profile_count);
    free(p->repo); free(p->url); free(p->session); free(p->profile); free(p->want); free(p->asked); free(p->branch);
    free(p->serve_error); free(p->delete_error); free(p->log_session);
    free(p->access_id); free(p->access_secret); free(p->access_suffix);
    free(p);
}
