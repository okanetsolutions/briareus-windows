#include "screen_findings_internal.h"

static void projects_done(void *owner, Request *req);
static void cycle_start(FindingsScreen *s);
static void cycle_end(FindingsScreen *s);
static void restore(FindingsScreen *s);
static void open_session_by_id(FindingsScreen *s, const char *sid);
static void set_all(FindingsScreen *s, size_t r, const char *decision);
static void findings_action(Screen *base, int action, intptr_t arg, POINT pt);
static void findings_timer(Screen *base, UINT id);
static void findings_visible(Screen *base, bool shown);
static void findings_refresh(Screen *base);
static void findings_activated(Screen *base, bool active);
static void findings_destroy(Screen *base);

// MARK: - Rounds and their drafts

int screen_findings_round_number(const Json *held) { return json_int_or(json_get(held, "round"), 0); }

const Session *screen_findings_round_session(FindingsScreen *s, size_t round) { return &s->all[s->rounds[round].index]; }

static void sessions_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    Session *items; size_t n;
    if (req->ok && sessions_parse(req->result, &items, &n)) {
        // One list for every project; each project's own rows are saved under its key.
        for (size_t p = 0; p < s->project_count; p++) {
            const char *repo = s->projects[p].repo;
            Json *list = json_array();
            for (size_t i = 0; i < n; i++) {
                if (!str_eq(session_repo(&items[i]), repo)) continue;
                json_array_push(list, json_clone(items[i].raw));
                s->incoming = xrealloc(s->incoming, (s->incoming_count + 1) * sizeof *s->incoming);
                session_copy(&s->incoming[s->incoming_count++], &items[i]);
            }
            char *key = xstrfmt("sessions:%s", repo);
            cache_store(g_store.cache, list, key);
            json_free(list); free(key);
        }
        sessions_free(items, n);
    } else {
        // A refusal keeps what was shown before rather than emptying the cards mid-edit.
        for (size_t i = 0; i < s->all_count; i++) {
            s->incoming = xrealloc(s->incoming, (s->incoming_count + 1) * sizeof *s->incoming);
            session_copy(&s->incoming[s->incoming_count++], &s->all[i]);
        }
        char *text = request_error_or_unexpected(req);
        set_string(&s->error, text); free(text);
        s->cycle_failed = true; s->retry_after = req->error.retry_after;
    }
    cycle_end(s);
}

static void projects_done(void *owner, Request *req) {
    FindingsScreen *s = owner;
    Project *items; size_t n;
    if (req->ok && projects_parse(req->result, &items, &n)) {
        projects_free(s->projects, s->project_count);
        s->projects = items; s->project_count = n;
        Json *list = projects_json(items, n); cache_store(g_store.cache, list, "projects"); json_free(list);
        store_call("sessions", json_object(), 0, s, sessions_done, TAG_SESSIONS, &s->req_sessions);
        return;
    }
    request_error_into(&s->error, req);
    s->cycle_failed = true; s->projects_failed = true; s->retry_after = req->error.retry_after;
    cycle_end(s);
}

static void cycle_start(FindingsScreen *s) {
    if (s->req_projects || s->req_sessions) return;
    s->cycle_failed = false; s->projects_failed = false; s->retry_after = -1;
    sessions_free(s->incoming, s->incoming_count); s->incoming = NULL; s->incoming_count = 0;
    store_call("projects", json_object(), 0, s, projects_done, TAG_PROJECTS, &s->req_projects);
}

/// Reads everything again now, as after a write: what the server holds is the answer to what was sent.
void screen_findings_cycle_now(FindingsScreen *s) {
    request_cancel(&s->req_projects); request_cancel(&s->req_sessions);
    cycle_start(s);
}

static void cycle_end(FindingsScreen *s) {
    if (!s->projects_failed) {
        sessions_free(s->all, s->all_count);
        s->all = s->incoming; s->all_count = s->incoming_count;
        s->incoming = NULL; s->incoming_count = 0;
        screen_findings_rebuild(s);
    }
    if (!s->cycle_failed) set_string(&s->error, NULL);
    s->loaded = true;
    // A completion that failed because its round is no longer held has no card left to carry its error.
    if (s->unsent) {
        Card *c = screen_findings_card_find(s, s->unsent);
        if (c && c->error && screen_findings_find_round(s, s->unsent, s->unsent_round) < 0) {
            const char *title = NULL;
            for (size_t i = 0; i < s->all_count && !title; i++) if (str_eq(session_id(&s->all[i]), s->unsent)) title = session_display_title(&s->all[i]);
            char *text = xstrfmt("Not sent: %s", c->error);
            screen_findings_outcome_set(s, s->unsent, title ? title : s->unsent_title, text, true);
            free(text);
            set_string(&c->error, NULL);
        }
        set_string(&s->unsent, NULL); set_string(&s->unsent_title, NULL);
    }
    poller_finished(&s->poller, s->cycle_failed, s->retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    projects_recount_findings();
}

/// What was saved opens at once; the server is asked for the rest.
static void restore(FindingsScreen *s) {
    Json *saved = cache_value(g_store.cache, "projects");
    if (!saved) return;
    Project *items; size_t n;
    if (projects_parse(saved, &items, &n)) {
        projects_free(s->projects, s->project_count);
        s->projects = items; s->project_count = n;
        for (size_t i = 0; i < n; i++) {
            char *key = xstrfmt("sessions:%s", items[i].repo);
            Json *list = cache_value(g_store.cache, key);
            free(key);
            if (!list) continue;
            Session *sessions; size_t m;
            if (sessions_parse(list, &sessions, &m)) {
                s->all = xrealloc(s->all, (s->all_count + m + 1) * sizeof *s->all);
                for (size_t k = 0; k < m; k++) session_copy(&s->all[s->all_count++], &sessions[k]);
                sessions_free(sessions, m);
            }
            json_free(list);
        }
        s->loaded = true;
        screen_findings_rebuild(s);
    }
    json_free(saved);
}

size_t findings_waiting(const Project *projects, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        char *key = xstrfmt("sessions:%s", projects[i].repo);
        Json *list = cache_value(g_store.cache, key);
        free(key);
        if (!list) continue;
        Session *sessions; size_t m;
        if (sessions_parse(list, &sessions, &m)) {
            for (size_t k = 0; k < m; k++) if (session_held_round(&sessions[k])) total++;
            sessions_free(sessions, m);
        }
        json_free(list);
    }
    return total;
}

// MARK: - Actions

static void open_session_by_id(FindingsScreen *s, const char *sid) {
    for (size_t i = 0; i < s->all_count; i++) if (str_eq(session_id(&s->all[i]), sid)) { app_push_from(&s->base, conversation_screen_new(&s->all[i])); return; }
}

static void set_all(FindingsScreen *s, size_t r, const char *decision) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held, *findings = json_get(held, "findings");
    int round = screen_findings_round_number(held);
    for (size_t k = 0; k < json_count(findings); k++) {
        const char *key = json_str(json_get(json_at(findings, k), "key"));
        if (key) set_string(&screen_findings_draft_get(s, session_id(ses), round, key)->decision, decision);
    }
    screen_findings_card_forget_saved(s, session_id(ses));
}

static void findings_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    FindingsScreen *s = (FindingsScreen *)base;
    if (action >= SSH_APPROVE && action <= SSH_REFRESH) { ssh_approvals_action(s->ssh, action, arg); return; }
    if (action == ACT_DISMISS_OUTCOME) { screen_findings_outcome_drop(s, (size_t)arg); pane_relayout(base->pane); return; }
    if (action == ACT_OUTCOME_SESSION) {
        if ((size_t)arg >= s->outcome_count) return;
        char *sid = xstrdup(s->outcomes[arg].session_id);
        screen_findings_outcome_drop(s, (size_t)arg);
        open_session_by_id(s, sid);
        free(sid);
        pane_relayout(base->pane);
        return;
    }
    // A click that lands between a poll and the redraw is on rounds the screen no longer shows: nothing is drafted for it.
    if (s->doc_stale) return;
    size_t r, k = 0; int segment = -1;
    if (action == ACT_VERDICT) { segment = (int)(arg % 4); arg /= 4; }
    switch (action) {
    case ACT_VERDICT: case ACT_COMMENT: case ACT_REPLY: case ACT_DELETE: case ACT_OPEN_FINDING: case ACT_REPLIED_URL:
        r = (size_t)arg / FINDING_STRIDE; k = (size_t)arg % FINDING_STRIDE; break;
    default: r = (size_t)arg; break;
    }
    if (r >= s->round_count) return;
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), k);
    const char *key = json_str(json_get(f, "key"));
    int round = screen_findings_round_number(held);
    switch (action) {
    case ACT_OPEN_SESSION: app_push_from(&s->base, conversation_screen_new(ses)); break;
    case ACT_OPEN_PR: { char *url = held_round_pr_url(ses, held); open_web_url(url); free(url); break; }
    case ACT_OPEN_FINDING: {
        const char *url = json_str(json_get(f, "url"));
        if (safe_web_url(url)) open_web_url(url);
        else { char *pr = held_round_pr_url(ses, held); if (pr) { char *files = xstrfmt("%s/files", pr); open_web_url(files); free(files); } free(pr); }
        break;
    }
    case ACT_REPLIED_URL: { const Replied *rp = key ? screen_findings_replied_find(s, session_id(ses), key) : NULL; if (rp && rp->url) open_web_url(rp->url); break; }
    case ACT_SAVED_URL: { const Card *c = screen_findings_card_find(s, session_id(ses)); if (c && c->saved_url) open_web_url(c->saved_url); break; }
    case ACT_VERDICT: {
        if (!key || segment < 0 || segment >= FINDING_DECISION_COUNT) break;
        Draft *d = screen_findings_draft_get(s, session_id(ses), round, key);
        set_string(&d->decision, str_eq(d->decision, finding_decision_ids[segment]) ? NULL : finding_decision_ids[segment]);   // the same pick twice clears it
        screen_findings_card_forget_saved(s, session_id(ses));
        pane_relayout(base->pane);
        break;
    }
    case ACT_COMMENT: {
        if (!key) break;
        char *sid = xstrdup(session_id(ses)), *fkey = xstrdup(key);
        const char *ft = json_str(json_get(f, "title"));
        char *typed = dialog_text(app_dialog_owner(), "Comment on this finding", ft ? ft : "Comment", "Save", screen_findings_reason_of(s, sid, round, f));
        if (typed && screen_findings_find_round(s, sid, round) >= 0) { set_string(&screen_findings_draft_get(s, sid, round, fkey)->reason, typed); screen_findings_card_forget_saved(s, sid); pane_relayout(base->pane); }
        free(typed); free(sid); free(fkey);
        break;
    }
    case ACT_REPLY: screen_findings_reply_on(s, r, k); break;
    case ACT_DELETE: screen_findings_delete_finding(s, r, k); break;
    case ACT_FIX_ALL: set_all(s, r, "fix"); pane_relayout(base->pane); break;
    case ACT_CLEAR_ALL: set_all(s, r, NULL); pane_relayout(base->pane); break;
    case ACT_NOTE: {
        char *sid = xstrdup(session_id(ses));
        char *typed = dialog_text(app_dialog_owner(), "Note for the fix session", "A note for the pull request and the fix session", "Save", screen_findings_note_of(s, sid, round));
        if (typed && screen_findings_find_round(s, sid, round) >= 0) { screen_findings_note_set(s, sid, round, typed); screen_findings_card_forget_saved(s, sid); pane_relayout(base->pane); }
        free(typed); free(sid);
        break;
    }
    case ACT_SAVE: screen_findings_save_round(s, r); break;
    case ACT_COMPLETE: screen_findings_complete_round(s, r); break;
    }
}

// MARK: - Lifecycle

static void findings_timer(Screen *base, UINT id) {
    FindingsScreen *s = (FindingsScreen *)base;
    if (!poller_fired(&s->poller, id)) return;
    ssh_approvals_refresh(s->ssh);
    // A read still under way answers this tick too; the poller only needs its next one scheduled.
    if (s->req_projects || s->req_sessions) poller_finished(&s->poller, false, -1); else cycle_start(s);
}

static void findings_visible(Screen *base, bool shown) {
    FindingsScreen *s = (FindingsScreen *)base;
    ssh_approvals_visible(s->ssh, shown);
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 7000);
    else { poller_stop(&s->poller); request_cancel(&s->req_projects); request_cancel(&s->req_sessions); sessions_free(s->incoming, s->incoming_count); s->incoming = NULL; s->incoming_count = 0; }
}

static void findings_refresh(Screen *base) { FindingsScreen *s = (FindingsScreen *)base; ssh_approvals_refresh(s->ssh); screen_findings_cycle_now(s); }

static void findings_activated(Screen *base, bool active) { if (active) { FindingsScreen *s = (FindingsScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 7000); } }

static void findings_destroy(Screen *base) {
    FindingsScreen *s = (FindingsScreen *)base;
    poller_stop(&s->poller);
    ssh_approvals_free(s->ssh);
    request_cancel(&s->req_projects); request_cancel(&s->req_sessions); request_cancel(&s->req_write);
    projects_free(s->projects, s->project_count);
    sessions_free(s->all, s->all_count); sessions_free(s->incoming, s->incoming_count);
    free(s->rounds); free(s->error);
    for (size_t i = 0; i < s->draft_count; i++) screen_findings_draft_clear(&s->drafts[i]);
    free(s->drafts);
    for (size_t i = 0; i < s->note_count; i++) { free(s->notes[i].session_id); free(s->notes[i].note); }
    free(s->notes);
    str_array_free(s->seeded, s->seeded_count);
    for (size_t i = 0; i < s->replied_count; i++) screen_findings_replied_clear(&s->replied[i]);
    free(s->replied);
    for (size_t i = 0; i < s->card_count; i++) screen_findings_card_clear(&s->cards[i]);
    free(s->cards);
    for (size_t i = 0; i < s->outcome_count; i++) screen_findings_outcome_clear(&s->outcomes[i]);
    free(s->outcomes);
    free(s->write_session); free(s->write_key); free(s->write_title); free(s->unsent); free(s->unsent_title);
    screen_release(base);
}

static const ScreenVTable findings_vt = {
    .destroy = findings_destroy, .layout = screen_findings_findings_layout, .header = screen_findings_findings_header, .action = findings_action,
    .timer = findings_timer, .visible = findings_visible, .refresh = findings_refresh, .activated = findings_activated,
    .detachable = true,
};

Screen *findings_screen_new(void) {
    FindingsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &findings_vt; s->base.id = xstrdup("findings");
    s->ssh = ssh_approvals_new(&s->base);
    s->write_kind = WRITE_NONE; s->retry_after = -1;
    restore(s);
    return &s->base;
}
