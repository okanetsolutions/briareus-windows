#include "screen_pulls_internal.h"

static void run_forget(PullScreen *s);
static void log_follow(PullScreen *s, const char *session);
static void log_events_done(void *owner, Request *req);
static void log_find_done(void *owner, Request *req);
static void show_run(PullScreen *s, const char *url);
static void profiles_done(void *owner, Request *req);
static void run_done(void *owner, Request *req);
static bool run_adopt(PullScreen *s);

/// The trash button at the right of a session row: muted, red with a tint under the pointer, an ellipsis while deleting.
void screen_pulls_paint_delete_run(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool deleting = it->data != NULL;
    bool hovered = it->action && doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(6), blend(theme.danger, theme.elevated, 0.14), blend(theme.danger, theme.elevated, 0.14));
    RECT r = *rc;
    if (deleting) draw_text(cv, "\xE2\x80\xA6", &r, FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else draw_glyph(cv, 0xE74D, rc, FONT_ICON_SMALL, hovered ? theme.danger : (it->action ? theme.muted : theme.line_strong));
}

void screen_pulls_layout_runs(PullScreen *s, Doc *doc, Col c) {
    if (s->run_error) { doc_notice(doc, c.x, c.w, s->run_error); doc_space(doc, px(8)); }
    if (!s->run_count) { doc_text(doc, c.x, c.w, "No conversations on this pull request", FONT_CALLOUT, theme.secondary, DT_SINGLELINE); return; }
    bool deletable = store_supports("delete");
    int bw = px(28);
    for (size_t i = 0; i < s->run_count; i++) {
        int box = doc_session_row(doc, c.x, c.w, &s->runs[i], ACT_OPEN_RUN, (intptr_t)i, false, theme.elevated, deletable ? bw + px(8) : 0);
        if (deletable) {
            // Laid after the row, so it is the topmost item there and takes the click instead of the row.
            RECT row = doc_item(doc, box)->rc;
            int mid = (row.top + row.bottom) / 2;
            RECT br = { row.right - px(10) - bw, mid - bw / 2, row.right - px(10), mid + bw / 2 };
            int di = doc_add(doc, &br, screen_pulls_paint_delete_run);
            Item *it = doc_item(doc, di);
            bool mine = s->deleting_run && str_eq(s->deleting_run, session_id(&s->runs[i]));
            it->data = mine ? s : NULL;   // borrowed owner marks the pending deletion
            if (!s->deleting_run) { it->action = ACT_DELETE_RUN; it->arg = (intptr_t)i; it->hand = true; }
        }
        doc_space(doc, px(6));
    }
}

/// The Run tab's session was deleted, and the workspace serving it with it: the tab forgets it and gives way to the PR
/// body; opening it again prepares a new one.
static void run_forget(PullScreen *s) {
    request_cancel(&s->req_run); request_cancel(&s->req_log);
    s->run_busy = false; s->run_pending = false;
    set_string(&s->run_session, NULL); set_string(&s->run_url, NULL); set_string(&s->run_profile, NULL);
    set_string(&s->run_asked, NULL); set_string(&s->serve_error, NULL);
    set_string(&s->log_session, NULL); run_log_clear(&s->log);
    webview_free(s->web); s->web = NULL;
    if (s->tab == PR_TAB_RUN) s->tab = PR_TAB_BODY;
    if (s->base.pane) { KillTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG); pane_header_changed(s->base.pane); }
}

void screen_pulls_delete_run_done(void *owner, Request *req) {
    PullScreen *s = owner;
    const char *id = json_str(json_get(req->args, "sessionId"));
    if (req->ok) {
        set_string(&s->run_error, NULL);
        if (str_eq(id, s->run_session) || str_eq(id, s->log_session)) run_forget(s);
        // The sidebar drops the row now instead of at its next poll, and so does this list.
        sessions_forget(s->project.repo, id);
        for (size_t i = 0; i < s->run_count; i++) {
            if (!str_eq(session_id(&s->runs[i]), id)) continue;
            session_free(&s->runs[i]);
            memmove(&s->runs[i], &s->runs[i + 1], (s->run_count - i - 1) * sizeof *s->runs);
            s->run_count--;
            break;
        }
    } else { request_error_into(&s->run_error, req); }
    set_string(&s->deleting_run, NULL);
    // A list read while the delete was in flight may still hold the row: read it again.
    if (store_supports("sessions")) {
        request_cancel(&s->req_sessions);
        Json *a = json_object(); json_set_str(a, "repo", s->project.repo);
        store_call("sessions", a, 0, s, screen_pulls_sessions_done_pull, TAG_SESSIONS, &s->req_sessions);
    }
    pane_relayout(s->base.pane);
}

/// The profile the tab shows as chosen: the one picked, else the one served, else the project's default.
const char *screen_pulls_run_shown_profile(PullScreen *s) {
    if (s->run_want) return s->run_want;
    if (s->run_profile) return s->run_profile;
    return s->profile_count ? s->profiles[0] : NULL;
}

/// The Run tab: the browser's area under its address bar, down to the bottom of the pane, with what is happening
/// written in it until the page is up. The tab itself is the run profile dropdown.
void screen_pulls_layout_run(PullScreen *s, Doc *doc, int w) {
    RECT view = pane_content_rect(s->base.pane);
    if (s->run_error) { doc_notice(doc, px(4), w - px(8), s->run_error); doc_space(doc, px(10)); }
    bool page = s->run_url && !s->run_busy && s->web && webview_ready(s->web);
    if (page && s->serve_error) { doc_text(doc, px(4), w - px(8), s->serve_error, FONT_FOOTNOTE, theme.danger, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(10)); }
    if (s->run_url && !s->run_busy) {
        static const int actions[4] = { ACT_WEB_BACK, ACT_WEB_FORWARD, ACT_WEB_RELOAD, ACT_WEB_BROWSER };
        run_browser_bar(doc, w, s->web, s->run_url, actions, &s->address);
    }
    int area = doc->y, h = (view.bottom - view.top) - area - px(12);
    if (h < px(320)) h = px(320);
    SetRect(&s->web_rc, 0, area, w, area + h);
    if (page) { doc->y = area + h; return; }
    const char *error = s->run_busy ? NULL : !s->run_url ? s->serve_error : s->web ? webview_error(s->web) : NULL;
    char *text = error ? xstrdup(error)
        : s->run_busy && s->run_url ? (s->run_asked ? xstrfmt("Restarting with profile %s\xE2\x80\xA6", s->run_asked) : xstrdup("Serving it again\xE2\x80\xA6"))
        : s->run_url ? xstrdup("Starting the browser\xE2\x80\xA6")
        : s->run_busy && s->run_session ? xstrdup("Serving it\xE2\x80\xA6")
        // Once the setup's console has lines it says what is happening; until then, a line saying what is coming.
        : s->log.count ? NULL
        : xstrdup("Preparing a workspace for this pull request and serving it with the project\xE2\x80\x99s run commands. This can take a few minutes\xE2\x80\xA6");
    if (text) doc_text(doc, px(4), w - px(8), text, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    free(text);
    if (!s->run_url && s->serve_error && !s->run_busy) {
        doc_space(doc, px(10));
        doc_button(doc, px(4), 0, "\xE2\x96\xB6 Try again", BUTTON_BORDERED, ACT_RUN_RETRY, 0, true);
    } else if (error && s->run_url) {
        doc_space(doc, px(8));
        int i = doc_text(doc, px(4), w - px(8), "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
        doc_item(doc, i)->action = ACT_WEB_BROWSER; doc_item(doc, i)->hand = true;
    }
    // The setup as it happens, its latest lines filling what is left of the area, as a terminal does.
    if (s->log.count && (s->run_busy || !s->run_url)) {
        doc_space(doc, px(12));
        int line = px(17), room = area + h - doc->y - px(24);
        size_t fit = room > line ? (size_t)(room / line) : 1, first = s->log.count > fit ? s->log.count - fit : 0;
        int box = doc_box_begin(doc, px(4), w - px(8), px(10), theme.sunken, theme.line, px(6));
        doc_item(doc, box)->hover_fill = false;
        for (size_t i = first; i < s->log.count; i++)
            doc_text(doc, px(16), w - px(32), s->log.lines[i], FONT_MONO_SMALL, s->log.errors[i] ? theme.danger : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        doc_box_end(doc, box, px(10));
    }
    if (doc->y < area + h) doc->y = area + h;
}

static void log_follow(PullScreen *s, const char *session) {
    if (str_eq(s->log_session, session)) return;
    set_string(&s->log_session, session);
    run_log_clear(&s->log);
}

static void log_events_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok || !str_eq(json_str(json_get(req->args, "sessionId")), s->log_session)) return;
    if (run_log_add_events(&s->log, json_get(req->result, "events")) && s->tab == PR_TAB_RUN && s->base.pane) pane_relayout(s->base.pane);
}

/// While `serve_pull` prepares the session, its id is not known yet: the newest ▶ Run session on this pull request is it.
static void log_find_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok || s->run_session) return;
    char *id = run_session_preparing(req->result, s->number);
    if (id) log_follow(s, id);
    free(id);
}

void screen_pulls_run_log_tick(PullScreen *s) {
    if (!s->run_busy) { if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG); return; }
    if (s->req_log) return;
    if (s->run_session) log_follow(s, s->run_session);
    if (s->log_session && store_supports("session")) {
        Json *a = json_object(); json_set_str(a, "sessionId", s->log_session); json_set_num(a, "since", s->log.cursor);
        store_call("session", a, 0, s, log_events_done, TAG_RUN_LOG, &s->req_log);
    } else if (!s->run_session && store_supports("sessions")) {
        Json *a = json_object(); json_set_str(a, "repo", s->project.repo);
        store_call("sessions", a, 0, s, log_find_done, TAG_RUN_LOG, &s->req_log);
    }
}

void screen_pulls_run_changed(PullScreen *s) { if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); } }

void screen_pulls_web_changed(void *ctx) {
    PullScreen *s = ctx;
    if (s->base.pane && pane_top(s->base.pane) == &s->base) screen_pulls_run_changed(s);
}

/// Shows the served address in the Run tab, starting the browser again when the address changed.
static void show_run(PullScreen *s, const char *url) {
    if (!str_eq(s->run_url, url)) { webview_free(s->web); s->web = NULL; set_string(&s->run_url, url); }
    screen_pulls_run_changed(s);
}

static void profiles_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    s->profiles_read = true;
    str_array_free(s->profiles, s->profile_count);
    s->profiles = run_profiles_parse(req->result, s->project.repo, &s->profile_count);
    screen_pulls_run_changed(s);
}

static void run_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->run_busy = false;
    bool switched = json_str(json_get(req->args, "sessionId")) != NULL;
    char *asked = s->run_asked; s->run_asked = NULL;
    if (!req->ok) {
        request_error_into(&s->serve_error, req);
        if (switched && (req->error.status == 404 || (req->error.message && strstr(req->error.message, "no live workspace")))) {
            // The session closed or expired, and its page with it: the next try prepares a new one.
            set_string(&s->run_session, NULL); set_string(&s->run_url, NULL);
            webview_free(s->web); s->web = NULL;
        } else if (switched) {
            // A refused switch leaves the app serving what it served.
            set_string(&s->run_want, s->run_profile);
        }
    } else {
        const char *id = json_str(json_get(json_get(req->result, "session"), "id"));
        if (id) set_string(&s->run_session, id);
        set_string(&s->run_profile, json_str(json_get(req->result, "profile")));
        const char *url = json_str(json_get(req->result, "url"));
        if (safe_web_url(url)) {
            bool same = str_eq(url, s->run_url);
            show_run(s, url);
            // The same address after a restart is a new app behind it.
            if (same && s->web) webview_reload(s->web);
        } else set_string(&s->serve_error, "The server did not say where it serves the pull request.");
        // A profile picked while this request was out, or a new session served with the default.
        if (s->run_want && s->run_session && !str_eq(s->run_want, asked) && !str_eq(s->run_want, s->run_profile)) { free(asked); screen_pulls_run_start(s); return; }
        if (store_supports("sessions")) { request_cancel(&s->req_sessions); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, screen_pulls_sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    }
    free(asked);
    screen_pulls_run_changed(s);
}

/// Serves the pull request: the run's session again, with the picked profile, when there is one; else a new session,
/// which the server serves with the project's default profile (switched afterwards when another was picked).
void screen_pulls_run_start(PullScreen *s) {
    if (s->run_busy) return;
    s->run_busy = true; set_string(&s->serve_error, NULL);
    Json *args = json_object();
    const char *op = "serve_pull";
    if (s->run_session && store_supports("serve")) {
        op = "serve";
        json_set_str(args, "sessionId", s->run_session);
        if (s->run_want) json_set_str(args, "profile", s->run_want);
        set_string(&s->run_asked, s->run_want);
    } else { json_set_str(args, "repo", s->project.repo); json_set_num(args, "prNumber", s->number); }
    store_call(op, args, 170000, s, run_done, TAG_RUN, &s->req_run);
    if (s->run_asked) { char *t = xstrfmt("\xE2\x96\xB6 Switching to profile %s", s->run_asked); run_log_add(&s->log, t, false); free(t); }
    if (s->base.pane) SetTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG, 1500, NULL);
    screen_pulls_run_changed(s);
}

/// A Run already serving this pull request (one of its sessions with serve links) is shown as it is.
static bool run_adopt(PullScreen *s) {
    Json *rows = json_array();
    for (size_t i = 0; i < s->run_count; i++) json_array_push(rows, json_clone(s->runs[i].raw));
    char *id = NULL, *url = NULL;
    bool found = run_session_serving(rows, s->number, &id, &url);
    json_free(rows);
    if (!found) return false;
    set_string(&s->run_session, id);
    show_run(s, url);
    free(id); free(url);
    return true;
}

void screen_pulls_run_resume(PullScreen *s) { if (!s->run_url && !s->run_busy && !s->serve_error && !run_adopt(s)) screen_pulls_run_start(s); }

/// The Run tab was opened: reads the project's run profiles once, and serves the pull request unless it is served.
void screen_pulls_profiles_load(PullScreen *s) {
    if (!s->profiles_read && !s->req_profiles && store_supports("projects") && store_supports("serve_pull")) store_call("projects", json_object(), 0, s, profiles_done, TAG_PROFILES, &s->req_profiles);
}

void screen_pulls_run_open(PullScreen *s) {
    s->tab = PR_TAB_RUN;
    screen_pulls_profiles_load(s);
    // Until the Sessions list is read, a Run serving this pull request may be in it.
    if (store_supports("sessions") && !s->runs_read) s->run_pending = true;
    else screen_pulls_run_resume(s);
    screen_pulls_run_changed(s);
}

void screen_pulls_run_pick_profile(PullScreen *s, POINT pt) {
    if (!s->profile_count) return;
    HMENU menu = CreatePopupMenu();
    const char *shown = screen_pulls_run_shown_profile(s);
    for (size_t i = 0; i < s->profile_count; i++) {
        char *label = i ? xstrdup(s->profiles[i]) : xstrfmt("%s (default)", s->profiles[i]);
        wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (str_eq(shown, s->profiles[i]) ? MF_CHECKED : 0), (UINT_PTR)(1 + i), wl);
        free(wl); free(label);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || (size_t)chosen > s->profile_count || str_eq(s->profiles[chosen - 1], shown)) return;
    set_string(&s->run_want, s->profiles[chosen - 1]);
    // While a request is out, its answer switches to the new pick.
    if (!s->run_busy) screen_pulls_run_start(s);
    screen_pulls_run_changed(s);
}
