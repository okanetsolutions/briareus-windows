#include "screen_status_internal.h"

bool status_current(StatusScreen *s) { return s->client == g_store.client && store_connected(); }

static StatusCard *card_for(StatusScreen *s, const Session *session) {
    for (size_t i = 0; i < s->card_count; i++) {
        StatusCard *c = s->cards[i];
        if (str_eq(session_id(&c->session), session_id(session)) && str_eq(session_repo(&c->session), session_repo(session))) return c;
    }
    StatusCard *c = xcalloc(1, sizeof *c); session_copy(&c->session, session);
    if (session_state(session) == SESSION_WAITING) {
        char *key = xstrfmt("transcript:%s", session_id(session));
        Json *saved = cache_lines(g_store.cache, key); free(key);
        transcript_append(&c->transcript, saved); json_free(saved);
    }
    s->cards = xrealloc(s->cards, (s->card_count + 1) * sizeof *s->cards); s->cards[s->card_count++] = c;
    return c;
}
void status_rebuild(StatusScreen *s) {
    status_cancel_questions(s);
    s->action_first = s->action_next; s->action_count = 0;
    for (size_t i = 0; i < s->card_count; i++) { s->cards[i]->present = false; s->cards[i]->confirmed = false; }
    sessions_free(s->sessions, s->count);
    s->sessions = session_cache_read(s->projects, s->project_count, &s->count, &s->missing);
    session_overview_free(&s->overview); s->overview = session_overview(s->sessions, s->count);
    for (size_t i = 0; i < s->count; i++) {
        const Session *session = &s->sessions[i];
        if (json_str_nonempty(json_get(session->raw, "parentId"))) continue;
        if (!session_needs_you(session) && session_state(session) != SESSION_WORKING) continue;
        StatusCard *c = card_for(s, session);
        Json *identity = session_question_identity(session);
        if (!json_equal(identity, c->identity)) {
            c->answered = false;
            // The saved transcript is still useful for its cursor; only a new server read may confirm the ask.
            json_free(c->identity); c->identity = identity;
        } else json_free(identity);
        session_free(&c->session); session_copy(&c->session, session); c->present = true;
    }
    s->next_question = 0;
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
static void sessions_done(void *owner, Request *req) {
    StatusScreen *s = owner;
    if (!status_current(s)) return;
    Session *items = NULL; size_t count = 0;
    bool ok = req->ok && sessions_parse(req->result, &items, &count);
    if (ok) { session_cache_save(s->projects, s->project_count, items, count); set_string(&s->error, NULL); }
    else { char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error); }
    sessions_free(items, count);
    s->loaded = true; status_rebuild(s); status_questions_next(s); projects_recount_findings();
    poller_finished(&s->poller, !ok, req->error.retry_after);
}
static void projects_done(void *owner, Request *req) {
    StatusScreen *s = owner;
    if (!status_current(s)) return;
    Project *items = NULL; size_t count = 0;
    if (req->ok && projects_parse(req->result, &items, &count)) {
        projects_free(s->projects, s->project_count); s->projects = items; s->project_count = count;
        cache_store(g_store.cache, req->result, "projects");
        // Drop inaccessible projects immediately, even when the following sessions request fails.
        status_rebuild(s);
        store_call("sessions", json_object(), 0, s, sessions_done, 0, &s->sessions_read);
    } else {
        char *error = request_error_or_unexpected(req); set_string(&s->error, error); free(error);
        s->loaded = true; poller_finished(&s->poller, true, req->error.retry_after); pane_relayout(s->base.pane);
    }
}
void status_load(StatusScreen *s) {
    if (!status_current(s) || s->projects_read || s->sessions_read) return;
    store_call("projects", json_object(), 0, s, projects_done, 0, &s->projects_read);
}
static void header(Screen *base, HeaderInfo *info) {
    StatusScreen *s = (StatusScreen *)base;
    snprintf(info->title, sizeof info->title, "\xE2\x97\x89 Status");
    if (status_current(s)) snprintf(info->subtitle, sizeof info->subtitle, "%u waiting for you · %u working", (unsigned)s->overview.waiting_count, (unsigned)s->overview.working_count);
}
static void action(Screen *base, int action, intptr_t arg, POINT point) {
    (void)point; StatusScreen *s = (StatusScreen *)base;
    if (!status_current(s) || arg < s->action_first || arg >= s->action_next) return;
    StatusAction a = s->actions[arg - s->action_first];
    if (a.card >= s->card_count) return;
    StatusCard *c = s->cards[a.card];
    if (!c->present) return;
    if (action == STATUS_OPEN) { app_push_from(base, conversation_screen_new(&c->session)); return; }
    if (action == STATUS_RESOLVE) {
        // Explicit acknowledgement of an uncertain write never resends the draft.
        c->uncertain = false; c->confirmed = false; set_string(&c->error, NULL); status_load(s); return;
    }
    const Event *question = transcript_pending_question(&c->transcript);
    if (action == STATUS_OPTION && question) {
        const char *label = json_str_nonempty(json_get(json_at(question->options, (size_t)a.option), "label"));
        status_send(s, a.card, label);
    } else if (action == STATUS_SEND) status_send(s, a.card, c->draft);
}
static void timer(Screen *base, UINT id) {
    StatusScreen *s = (StatusScreen *)base;
    if (!status_current(s)) { status_controls_hide(s); return; }
    if (!poller_fired(&s->poller, id)) return;
    if (s->projects_read || s->sessions_read) poller_finished(&s->poller, false, -1);
    else status_load(s);
}
static void visible(Screen *base, bool shown) {
    StatusScreen *s = (StatusScreen *)base; s->shown = shown;
    if (shown && status_current(s)) { poller_start(&s->poller, base->pane, STATUS_POLL, 7000); status_load(s); }
    else {
        poller_stop(&s->poller); request_cancel(&s->projects_read); request_cancel(&s->sessions_read); status_cancel_questions(s);
        if (s->write) { s->cards[s->sending_card]->uncertain = true; set_string(&s->cards[s->sending_card]->error, "Send cancelled locally; its outcome is unknown. Check the conversation before retrying."); request_cancel(&s->write); }
        status_controls_hide(s);
    }
}
static void activated(Screen *base, bool active) { if (active) visible(base, true); }
static void refresh(Screen *base) {
    StatusScreen *s = (StatusScreen *)base;
    request_cancel(&s->projects_read); request_cancel(&s->sessions_read); status_cancel_questions(s); status_load(s);
}
static void destroy(Screen *base) {
    StatusScreen *s = (StatusScreen *)base; visible(base, false);
    for (size_t i = 0; i < s->card_count; i++) {
        StatusCard *c = s->cards[i];
        if (c->edit) DestroyWindow(c->edit);
        session_free(&c->session); json_free(c->identity); json_free(c->answered_question); transcript_free(&c->transcript); free(c->draft); free(c->error); free(c->read_error); free(c);
    }
    free(s->cards); free(s->actions); free(s->error); free(s->send_text); json_free(s->send_question); json_free(s->send_identity);
    projects_free(s->projects, s->project_count); sessions_free(s->sessions, s->count); session_overview_free(&s->overview);
    api_client_release(s->client); screen_release(base);
}
static const ScreenVTable VT = {
    .destroy = destroy, .layout = status_layout, .header = header, .action = action, .timer = timer,
    .visible = visible, .activated = activated, .refresh = refresh, .place = status_place, .command = status_command, .detachable = true,
};
Screen *status_screen_new(void) {
    StatusScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT; s->base.id = xstrdup("status");
    s->client = g_store.client; api_client_retain(s->client);
    Json *saved = cache_value(g_store.cache, "projects");
    s->loaded = saved && projects_parse(saved, &s->projects, &s->project_count); json_free(saved);
    // Reads start on visibility.
    s->sessions = session_cache_read(s->projects, s->project_count, &s->count, &s->missing);
    s->overview = session_overview(s->sessions, s->count);
    status_rebuild(s);
    return &s->base;
}
