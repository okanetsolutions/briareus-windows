#include "screen_projects_internal.h"

static char *sessions_key(SessionsScreen *s);
static void sessions_show(SessionsScreen *s, const Json *value, bool from_server);
static void sessions_done(void *owner, Request *req);
static void sessions_load(SessionsScreen *s);
static bool is_picked(SessionsScreen *s, const char *id);
static bool session_open(const Session *ses);
static void bulk_done(void *owner, Request *req);
static void bulk_next(SessionsScreen *s);
static void sessions_destroy(Screen *base);
static void sessions_layout(Screen *base, Doc *doc);
static void sessions_header(Screen *base, HeaderInfo *info);
static void sessions_action(Screen *base, int action, intptr_t arg, POINT pt);
static void sessions_timer(Screen *base, UINT id);
static void sessions_visible(Screen *base, bool shown);
static void sessions_refresh(Screen *base);
static void sessions_activated(Screen *base, bool active);
static bool sessions_key_press(Screen *base, WPARAM vk, bool ctrl, bool shift);

static char *sessions_key(SessionsScreen *s) { return xstrdup(s->base.id); }

static void sessions_show(SessionsScreen *s, const Json *value, bool from_server) {
    Session *items; size_t n;
    if (!sessions_parse(value, &items, &n)) return;
    if (from_server) {
        // Transcripts of conversations that are gone go with them.
        for (size_t i = 0; i < s->count; i++) {
            bool kept = false;
            for (size_t k = 0; k < n && !kept; k++) kept = str_eq(session_id(&s->sessions[i]), session_id(&items[k]));
            if (!kept) { char *key = xstrfmt("transcript:%s", session_id(&s->sessions[i])); cache_remove(g_store.cache, key); free(key); }
        }
    }
    sessions_free(s->sessions, s->count);
    s->sessions = items; s->count = n; s->loaded = true;
}

static void sessions_done(void *owner, Request *req) {
    SessionsScreen *s = owner;
    if (req->ok) {
        sessions_show(s, req->result, true);
        set_string(&s->error, NULL);
        char *key = sessions_key(s);
        Json *list = sessions_json(s->sessions, s->count);
        if (s->all_projects) screen_projects_cache_all_sessions(s->sessions, s->count);
        else cache_store(g_store.cache, list, key);
        json_free(list); free(key);
        projects_recount_findings();
    } else {
        request_error_into(&s->error, req);
        s->loaded = true;
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane);
}

static void sessions_load(SessionsScreen *s) {
    if (!s->loaded) {
        char *key = sessions_key(s);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { sessions_show(s, saved, false); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
    }
    if (s->req) return;
    Json *args = json_object();
    if (!s->all_projects) json_set_str(args, "repo", s->project.repo);
    store_call("sessions", args, 0, s, sessions_done, 0, &s->req);
}

void sessions_forget(const char *repo, const char *id) {
    if (str_empty(id)) return;
    // The saved list, so a restart does not bring the conversation back.
    screen_projects_forget_cached_session(repo, id);
    // The list on screen, right away rather than at its next poll.
    Pane *sidebar = app_sidebar_pane();
    Screen *top = sidebar ? pane_top(sidebar) : NULL;
    if (!top || top->vt->layout != sessions_layout) return;
    SessionsScreen *s = (SessionsScreen *)top;
    if (!s->all_projects && !str_eq(s->project.repo, repo)) return;
    for (size_t i = 0; i < s->count; i++) {
        if (!str_eq(session_id(&s->sessions[i]), id)) continue;
        session_free(&s->sessions[i]);
        memmove(&s->sessions[i], &s->sessions[i + 1], (s->count - i - 1) * sizeof *s->sessions);
        s->count--;
        break;
    }
    pane_relayout(s->base.pane);
    // And the server's word on it, in case the list changed in other ways too.
    request_cancel(&s->req); sessions_load(s);
}

static bool is_picked(SessionsScreen *s, const char *id) { for (size_t i = 0; i < s->picked_count; i++) if (str_eq(s->picked[i], id)) return true; return false; }

void screen_projects_toggle_pick(SessionsScreen *s, const char *id) {
    for (size_t i = 0; i < s->picked_count; i++) if (str_eq(s->picked[i], id)) { free(s->picked[i]); s->picked[i] = s->picked[--s->picked_count]; return; }
    s->picked = xrealloc(s->picked, (s->picked_count + 1) * sizeof *s->picked);
    s->picked[s->picked_count++] = xstrdup(id);
}

void screen_projects_clear_picks(SessionsScreen *s) { str_array_free(s->picked, s->picked_count); s->picked = NULL; s->picked_count = 0; }

/// Open = still holding a workspace and a database, the only kind Close has anything to release.
static bool session_open(const Session *ses) { const char *st = session_status(ses); return str_eq(st, "queued") || str_eq(st, "preparing") || str_eq(st, "running") || str_eq(st, "idle"); }

static void bulk_done(void *owner, Request *req) {
    SessionsScreen *s = owner;
    if (!req->ok) { request_error_into(&s->bulk_error, req); }
    else if (s->queue_delete) {
        const char *id = json_str(json_get(req->args, "sessionId"));
        const char *repo = s->project.repo;
        for (size_t i = 0; i < s->count; i++) if (str_eq(session_id(&s->sessions[i]), id)) { repo = session_repo(&s->sessions[i]); break; }
        sessions_forget(repo, id);
    }
    s->queue_done++;
    bulk_next(s);
}

static void bulk_next(SessionsScreen *s) {
    if (s->queue_done >= s->queue_count) {
        str_array_free(s->queue, s->queue_count); s->queue = NULL; s->queue_count = s->queue_done = 0;
        screen_projects_clear_picks(s);
        request_cancel(&s->req); sessions_load(s);
        pane_relayout(s->base.pane); pane_footer_changed(s->base.pane);
        return;
    }
    Json *args = json_object(); json_set_str(args, "sessionId", s->queue[s->queue_done]);
    store_call(s->queue_delete ? "delete" : "close", args, 0, s, bulk_done, 0, &s->req_bulk);
}

void screen_projects_bulk_start(SessionsScreen *s, bool del, bool all) {
    if (s->req_bulk) return;
    size_t n = 0;
    char **ids = xcalloc(s->count, sizeof *ids);
    for (size_t i = 0; i < s->count; i++) {
        const Session *ses = &s->sessions[i];
        if (!all && !is_picked(s, session_id(ses))) continue;
        if (!del && !session_open(ses)) continue;
        ids[n++] = xstrdup(session_id(ses));
    }
    if (!n) { str_array_free(ids, n); return; }
    char *title = del ? xstrfmt("Delete %zu conversation%s and their logs?", n, n == 1 ? "" : "s") : xstrfmt("Close %zu session%s?", n, n == 1 ? "" : "s");
    bool ok = app_confirm(title, del ? "This cannot be undone." : "Each one releases its workspace and database; the conversation stays readable.", del ? "Delete" : "Close", del);
    free(title);
    if (!ok) { str_array_free(ids, n); return; }
    set_string(&s->bulk_error, NULL);
    s->queue = ids; s->queue_count = n; s->queue_done = 0; s->queue_delete = del;
    bulk_next(s);
}

static void sessions_destroy(Screen *base) {
    SessionsScreen *s = (SessionsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_bulk); poller_stop(&s->poller);
    sessions_free(s->sessions, s->count); project_free(&s->project); free(s->error); free(s->bulk_error);
    screen_projects_clear_picks(s); str_array_free(s->queue, s->queue_count);
    screen_release(base);
}

static void sessions_layout(Screen *base, Doc *doc) {
    SessionsScreen *s = (SessionsScreen *)base;
    int w = doc->width;
    const char *selected = pane_selected_id(base->pane);
    screen_projects_sidebar_top(doc, w, selected);
    // `‹ All projects`, the current scope, then its conversations.
    doc_custom(doc, 0, w, px(30), screen_projects_paint_back_row, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(2));
    char *board_id = s->all_projects ? NULL : xstrfmt("pulls:%s", s->project.repo);
    screen_projects_doc_project_row(doc, w, s->all_projects ? "All" : project_title(&s->project), (int)s->count, false, !s->all_projects && str_eq(selected, board_id), false, px(35), s->all_projects ? 0 : ACT_BOARD, 0);
    free(board_id);
    doc_space(doc, px(4));
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    for (size_t i = 0; i < s->count; i++) {
        const Session *ses = &s->sessions[i];
        char *id = xstrfmt("conversation:%s", session_id(ses));
        bool lit = s->select_mode ? is_picked(s, session_id(ses)) : str_eq(selected, id);
        screen_projects_doc_dashboard_session_row(doc, w, ses, s->all_projects, lit, s->select_mode, is_picked(s, session_id(ses)), s->select_mode ? ACT_PICK : ACT_OPEN_SESSION, (intptr_t)i);
        free(id);
    }
    if (s->loaded && !s->count) doc_text(doc, px(8), w - px(16), "No conversations here yet.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading\xE2\x80\xA6");
    doc_space(doc, px(8));
}

static void sessions_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

static void sessions_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    SessionsScreen *s = (SessionsScreen *)base;
    if (screen_projects_sidebar_common_action(base->pane, action)) return;
    switch (action) {
    case ACT_BACK: pane_pop(base->pane); break;
    case ACT_BOARD: if (!s->all_projects) app_show_detail(pulls_screen_new(&s->project)); break;
    case ACT_NEW: { size_t n; const Project *all = projects_list(&n); app_show_detail(new_session_screen_new(s->all_projects ? NULL : &s->project, all, n)); break; }
    case ACT_OPEN_SESSION:
        if ((size_t)arg >= s->count) break;
        // Ctrl-clicking a row turns ☑ Select on with that row ticked, as the dashboard does.
        if (GetKeyState(VK_CONTROL) & 0x8000) { s->select_mode = true; screen_projects_toggle_pick(s, session_id(&s->sessions[arg])); pane_footer_changed(base->pane); break; }
        app_show_detail(conversation_screen_new(&s->sessions[arg]));
        break;
    case ACT_PICK: if ((size_t)arg < s->count) { screen_projects_toggle_pick(s, session_id(&s->sessions[arg])); pane_footer_changed(base->pane); } break;
    }
}

static void sessions_timer(Screen *base, UINT id) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (poller_fired(&s->poller, id)) sessions_load(s);
}

static void sessions_visible(Screen *base, bool shown) {
    SessionsScreen *s = (SessionsScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 7000); else { poller_stop(&s->poller); request_cancel(&s->req); }
}

static void sessions_refresh(Screen *base) { SessionsScreen *s = (SessionsScreen *)base; request_cancel(&s->req); sessions_load(s); }

static void sessions_activated(Screen *base, bool active) { if (active) { SessionsScreen *s = (SessionsScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 7000); } }

static bool sessions_key_press(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    SessionsScreen *s = (SessionsScreen *)base;
    if (vk == VK_ESCAPE && s->select_mode) { s->select_mode = false; screen_projects_clear_picks(s); pane_footer_changed(base->pane); return true; }
    return false;
}

static const ScreenVTable sessions_vt = {
    .destroy = sessions_destroy, .layout = sessions_layout, .header = sessions_header, .action = sessions_action,
    .timer = sessions_timer, .visible = sessions_visible, .refresh = sessions_refresh, .activated = sessions_activated,
    .footer_height = screen_projects_sessions_footer_height, .footer_paint = screen_projects_sessions_footer_paint, .footer_click = screen_projects_sessions_footer_click,
    .footer_press = screen_projects_sessions_footer_press, .footer_drag = screen_projects_sessions_footer_drag, .footer_wheel = screen_projects_sessions_footer_wheel, .key = sessions_key_press,
};

Screen *sessions_screen_new(const Project *project) {
    SessionsScreen *s = xcalloc(1, sizeof *s);
    s->all_projects = project == NULL;
    s->base.vt = &sessions_vt; s->base.id = project ? xstrfmt("sessions:%s", project->repo) : xstrdup("sessions");
    if (project) project_copy(&s->project, project);
    return &s->base;
}
