// A project's Run tab on its board, after Issues: the pull request's Run tab, on the project's default branch. Opening it
// serves the branch in a clean workspace with the project's run commands (`serve_branch`, no agent turn) and offers a link to it in
// the default browser, with the setup's console until the page is up. The header picks the run
// profile and deletes the run with its workspace.
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_PROFILE, A_BROWSER, A_RETRY, A_DELETE };

struct ProjectRun {
    char *repo;
    Screen *host;
    int base; UINT timer;
    // `session` serves the branch at `url` with `profile`; `want` is the profile picked in the header, `asked` the one
    // the request in flight asked for. `branch` is what the server served, once it said.
    char *url, *session, *profile, *want, *asked, *branch, *serve_error, *delete_error;
    bool busy, pending, sessions_read, deleting;   // pending: waits for the sessions, in case a Run already serves the branch
    char **profiles; size_t profile_count; bool profiles_read;   // the project's run profiles, the default first
    // The log of the Run under way, from its session's transcript: `log_session` is the session followed (found among the
    // project's sessions while `serve_branch` prepares it).
    char *log_session; RunLog log;
    bool shown;
    Request *req_run, *req_profiles, *req_log, *req_sessions, *req_delete;
};

bool project_run_offered(void) { return store_can_manage() && store_supports("serve_branch"); }

static void changed(ProjectRun *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static void timer_on(ProjectRun *p, bool on) {
    if (!p->host->pane) return;
    if (on) SetTimer(pane_hwnd(p->host->pane), p->timer, 1500, NULL);
    else KillTimer(pane_hwnd(p->host->pane), p->timer);
}
/// The session the tab shows: the one serving it, else the one being prepared for it.
static const char *target(ProjectRun *p) { return p->session ? p->session : p->log_session; }
/// The profile shown as chosen: the one picked, else the one served, else the project's default.
static const char *shown_profile(ProjectRun *p) {
    if (p->want) return p->want;
    if (p->profile) return p->profile;
    return p->profile_count ? p->profiles[0] : NULL;
}
/// Updates the served address.
static void show_url(ProjectRun *p, const char *url) {
    set_string(&p->url, url);
    changed(p);
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
    if (!p->busy) { timer_on(p, false); return; }
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

// MARK: - Serving

static void start(ProjectRun *p);
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
            show_url(p, url);
        } else set_string(&p->serve_error, "The server did not say where it serves the branch.");
        // A profile picked while this request was out, or a new session served with the default.
        if (p->want && p->session && !str_eq(p->want, asked) && !str_eq(p->want, p->profile)) { free(asked); start(p); return; }
    }
    free(asked);
    changed(p);
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
    timer_on(p, true);
    changed(p);
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
    changed(p);
}
static void profiles_done(void *owner, Request *req) {
    ProjectRun *p = owner;
    if (!req->ok) return;
    p->profiles_read = true;
    str_array_free(p->profiles, p->profile_count);
    p->profiles = run_profiles_parse(req->result, p->repo, &p->profile_count);
    changed(p);
}
void project_run_open(ProjectRun *p) {
    if (!p->profiles_read && !p->req_profiles && store_supports("projects")) store_call("projects", json_object(), 0, p, profiles_done, 0, &p->req_profiles);
    // Until the project's sessions are read, a Run serving the branch may be among them.
    if (!p->sessions_read && store_supports("sessions")) {
        p->pending = true;
        if (!p->req_sessions) { Json *a = json_object(); json_set_str(a, "repo", p->repo); store_call("sessions", a, 0, p, sessions_done, 0, &p->req_sessions); }
    } else resume(p);
    if (p->busy) timer_on(p, true);
    changed(p);
}
static void pick_profile(ProjectRun *p, POINT pt) {
    if (!p->profile_count) return;
    HMENU menu = CreatePopupMenu();
    const char *shown = shown_profile(p);
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
    changed(p);
}

// MARK: - Deleting

/// The run's session was deleted, and the workspace serving it with it: opening the tab again prepares a new one.
static void forget(ProjectRun *p) {
    request_cancel(&p->req_run); request_cancel(&p->req_log);
    p->busy = false;
    set_string(&p->session, NULL); set_string(&p->url, NULL); set_string(&p->profile, NULL);
    set_string(&p->asked, NULL); set_string(&p->serve_error, NULL);
    set_string(&p->log_session, NULL); run_log_clear(&p->log);
    timer_on(p, false);
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
    changed(p);
}
static void delete_run(ProjectRun *p) {
    if (!target(p) || p->deleting || !store_supports("delete")) return;
    char *id = xstrdup(target(p));
    bool ok = app_confirm("Delete this run?", "Its workspace stops serving the branch, and its conversation and transcript are deleted permanently.", "Delete", true);
    // The run may have changed while the dialog was open: delete the one asked about only if it is still shown.
    if (ok && !p->deleting && str_eq(id, target(p))) {
        p->deleting = true; set_string(&p->delete_error, NULL);
        Json *args = json_object(); json_set_str(args, "sessionId", id);
        store_call("delete", args, 0, p, delete_done, 0, &p->req_delete);
        changed(p);
    }
    free(id);
}

// MARK: - The tab

void project_run_layout(ProjectRun *p, Doc *doc, int w) {
    RECT view = pane_content_rect(p->host->pane);
    if (p->delete_error) { doc_notice(doc, 0, w, p->delete_error); doc_space(doc, px(10)); }
    if (p->url && !p->busy) {
        if (p->serve_error) { doc_notice(doc, 0, w, p->serve_error); doc_space(doc, px(10)); }
        doc_text(doc, 0, w, p->url, FONT_BODY, theme.secondary, DT_WORDBREAK | DT_NOPREFIX);
        doc_space(doc, px(8));
        doc_button(doc, 0, 0, "Open in your browser", BUTTON_BORDERED, p->base + A_BROWSER, 0, true);
        return;
    }
    int area = doc->y, h = (view.bottom - view.top) - area - px(12);
    if (h < px(320)) h = px(320);
    const char *error = p->busy ? NULL : p->serve_error;
    char *text = error ? xstrdup(error)
        : p->busy && p->url ? (p->asked ? xstrfmt("Restarting with profile %s\xE2\x80\xA6", p->asked) : xstrdup("Serving it again\xE2\x80\xA6"))
        : p->busy && p->session ? xstrdup("Serving it\xE2\x80\xA6")
        // Once the setup's console has lines it says what is happening; until then, a line saying what is coming.
        : p->log.count ? NULL
        : p->busy || p->pending ? xstrdup("Preparing a workspace on the default branch and serving it with the project\xE2\x80\x99s run commands. This can take a few minutes\xE2\x80\xA6")
        : NULL;
    if (text) doc_text(doc, 0, w, text, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    free(text);
    if (!p->url && p->serve_error && !p->busy) {
        doc_space(doc, px(10));
        doc_button(doc, 0, 0, "\xE2\x96\xB6 Try again", BUTTON_BORDERED, p->base + A_RETRY, 0, true);
    }
    // The setup as it happens, its latest lines filling what is left of the area, as a terminal does.
    if (p->log.count && (p->busy || !p->url)) {
        doc_space(doc, px(12));
        int line = px(17), room = area + h - doc->y - px(24);
        size_t fit = room > line ? (size_t)(room / line) : 1, first = p->log.count > fit ? p->log.count - fit : 0;
        int box = doc_box_begin(doc, 0, w, px(10), theme.sunken, theme.line, px(6));
        doc_item(doc, box)->hover_fill = false;
        for (size_t i = first; i < p->log.count; i++)
            doc_text(doc, px(12), w - px(24), p->log.lines[i], FONT_MONO_SMALL, p->log.errors[i] ? theme.danger : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        doc_box_end(doc, box, px(10));
    }
    if (doc->y < area + h) doc->y = area + h;
}
void project_run_header(ProjectRun *p, HeaderInfo *info) {
    snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s", p->repo, p->branch ? p->branch : "default branch");
    HeaderButton *b;
    if (p->profile_count) {
        b = &info->buttons[info->button_count++];
        snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", shown_profile(p)); b->action = p->base + A_PROFILE; b->enabled = true; b->tip = "The run profile it is served with";
    }
    if (store_supports("delete")) {
        b = &info->buttons[info->button_count++];
        b->glyph = 0xE74D; b->action = p->base + A_DELETE; b->enabled = target(p) && !p->deleting; b->destructive = true; b->tip = "Delete this run and its workspace";
    }
}
void project_run_place(ProjectRun *p, const RECT *content, int scroll_y, bool shown) {
    (void)content; (void)scroll_y;
    if (p->shown != shown) {
        p->shown = shown;
        timer_on(p, shown && p->busy);
    }
}
void project_run_refresh(ProjectRun *p) {
    if (p->busy || (p->url && !p->serve_error)) return;
    set_string(&p->serve_error, NULL); start(p);
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
    case A_BROWSER: if (p->url) open_web_url(p->url); break;
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
    request_cancel(&p->req_sessions); request_cancel(&p->req_delete);
    timer_on(p, false);
    run_log_clear(&p->log);
    str_array_free(p->profiles, p->profile_count);
    free(p->repo); free(p->url); free(p->session); free(p->profile); free(p->want); free(p->asked); free(p->branch);
    free(p->serve_error); free(p->delete_error); free(p->log_session);
    free(p);
}
