#include "screen_pulls_internal.h"

static void status_note(PullsScreen *s, int number, const Json *issue);
static void status_restore(PullsScreen *s);
static void status_done(void *owner, Request *req);
static void status_next(PullsScreen *s);
static void pulls_show(PullsScreen *s, const Json *result, bool saved);
static void board_actions_done(void *owner, Request *req);
static void runs_done(void *owner, Request *req);
static void board_start_done(void *owner, Request *req);
static void board_merge_done(void *owner, Request *req);
static void board_merge_read_done(void *owner, Request *req);

// ACT_PULL_ACTION's argument: row * stride + errand

// A list deadline must not replace the timer for sessions/actions and issue details.
void screen_pulls_list_retry_stop(Poller *poller, bool *armed) {
    if (poller->pane) KillTimer(pane_hwnd(poller->pane), TIMER_LIST_RETRY);
    *armed = false;
}

void screen_pulls_list_retry_arm(Poller *poller, bool *armed, const char *repo) {
    screen_pulls_list_retry_stop(poller, armed);
    time_t seconds = pulls_retry_deadline(repo) - time(NULL);
    if (!poller->running || !store_supports("pulls") || seconds <= 0) return;
    // Win32 caps timer intervals; long deadlines are checked again when the timer fires.
    UINT delay = seconds > USER_TIMER_MAXIMUM / 1000 ? USER_TIMER_MAXIMUM : (UINT)seconds * 1000;
    *armed = SetTimer(pane_hwnd(poller->pane), TIMER_LIST_RETRY, delay, NULL) != 0;
}

bool screen_pulls_list_retry_fired(Poller *poller, bool *armed, const char *repo, UINT id, bool paused) {
    if (id != TIMER_LIST_RETRY || !*armed) return false;
    screen_pulls_list_retry_stop(poller, armed);
    if (!poller->running || !store_supports("pulls")) return false;
    if (time(NULL) < pulls_retry_deadline(repo)) { screen_pulls_list_retry_arm(poller, armed, repo); return false; }
    pane_header_changed(poller->pane);
    if (!g_store.active || paused) {
        *armed = SetTimer(pane_hwnd(poller->pane), TIMER_LIST_RETRY, 2000, NULL) != 0;
        return false;
    }
    return true;
}

static void status_note(PullsScreen *s, int number, const Json *issue) {
    const char *st = screen_pulls_issue_project_status(issue);
    char *k = screen_pulls_number_key(number); json_set_str(s->issue_status, k, st ? st : ""); free(k);
}

/// What was saved of the linked issues not known yet, so the statuses show before they are read again.
static void status_restore(PullsScreen *s) {
    for (size_t i = 0; i < s->pull_count; i++) {
        for (size_t k = 0; k < s->pulls[i].issue_count; k++) {
            const BoardLink *l = &s->pulls[i].issues[k];
            char *nk = screen_pulls_number_key(l->number);
            bool known = board_link_is_foreign(l, s->project.repo) || json_str(json_get(s->issue_status, nk));
            free(nk);
            if (known) continue;
            char *key = screen_pulls_saved_issue_key(s->project.repo, l->number); Json *saved = cache_value(g_store.cache, key); free(key);
            if (saved) { status_note(s, l->number, json_get(saved, "issue")); json_free(saved); }
        }
    }
}

static void status_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    int number = json_int_or(json_get(req->args, "issue"), 0);
    // Throttling stops the round and leaves this issue unread, so it and the remaining reads wait for the
    // next poll rather than hitting the limit again.
    if (req->error.kind == API_HTTP && req->error.status == 429) return;
    char *k = screen_pulls_number_key(number); json_set_bool(s->status_read, k, true); free(k);
    if (req->ok) {
        status_note(s, number, json_get(req->result, "issue"));
        char *key = screen_pulls_saved_issue_key(s->project.repo, number); cache_store(g_store.cache, req->result, key); free(key);
        pane_relayout(s->base.pane);
    }
    status_next(s);
}

/// Reads the next linked issue not read this visit: those of the rows the filters show first, then the rest.
static void status_next(PullsScreen *s) {
    if (s->req_status || !s->shown || !store_supports("issue")) return;
    BoardRow *rows = xcalloc(s->pull_count ? s->pull_count : 1, sizeof *rows);
    for (size_t i = 0; i < s->pull_count; i++) rows[i] = pull_board_row(&s->pulls[i]);
    int next = 0;
    for (int pass = 0; pass < 2 && !next; pass++) {
        for (size_t i = 0; i < s->pull_count && !next; i++) {
            if (!pass && (!board_filter_passes(&s->pull_filter, &rows[i], -1) || !screen_pulls_stack_filter_passes(s, i))) continue;
            for (size_t k = 0; k < s->pulls[i].issue_count && !next; k++) {
                const BoardLink *l = &s->pulls[i].issues[k];
                char *nk = screen_pulls_number_key(l->number);
                if (!board_link_is_foreign(l, s->project.repo) && json_is_null(json_get(s->status_read, nk))) next = l->number;
                free(nk);
            }
        }
    }
    free(rows);
    if (!next) return;
    Json *a = json_object(); json_set_num(a, "issue", next); json_set_str(a, "repo", s->project.repo);
    store_call("issue", a, 0, s, status_done, 0, &s->req_status);
}

static void pulls_show(PullsScreen *s, const Json *result, bool saved) {
    json_free(s->board); s->board = json_clone(result);
    s->synced_at = pulls_sync_time(result, saved, time(NULL));
    if (!saved) json_set_num(s->board, "_receivedAt", (double)time(NULL));
    pull_summaries_free(s->pulls, s->pull_count); s->pulls = pull_summaries_parse(json_get(result, "pulls"), &s->pull_count);
    issue_summaries_free(s->issues, s->issue_count); s->issues = issue_summaries_parse(json_get(result, "issues"), &s->issue_count);
    board_tab_set_pulls(s->board_tab, s->issues, s->issue_count, s->pulls, s->pull_count);
    // A saved board may be out of date about who has something open, so the server's first answer
    // opens the board again, unless the pickers were touched meanwhile.
    if (s->has_opening) {
        size_t n; BoardRow *rows = xcalloc(s->pull_count, sizeof *rows);
        for (n = 0; n < s->pull_count; n++) rows[n] = pull_board_row(&s->pulls[n]);
        BoardFilter filter; board_filter_opening(&filter, json_str(json_get(result, "author")), rows, n);
        free(rows);
        if (board_filter_equal(&s->pull_filter, &s->opening)) { board_filter_free(&s->pull_filter); board_filter_copy(&s->pull_filter, &filter); }
        board_filter_free(&s->opening);
        if (saved) { board_filter_copy(&s->opening, &filter); } else { s->has_opening = false; board_filter_init(&s->opening); }
        board_filter_free(&filter);
    }
    if (s->tab == TAB_ISSUES && !s->issue_count && !json_is_set(json_get(result, "issuesError"))) s->tab = TAB_PULLS;
    status_restore(s);
    s->loaded = true;
}

/// The shared list read gate also covers the older-server retry without refreshing ancillary reads twice.
bool screen_pulls_pulls_read(const char *repo, bool fresh, void *owner, RequestDone done, Request **slot) {
    if (*slot || !store_supports("pulls") || time(NULL) < pulls_retry_deadline(repo)) return false;
    Json *args = json_object(); json_set_str(args, "repo", repo);
    if (fresh) json_set_str(args, "fresh", "1");
    store_call("pulls", args, 0, owner, done, 0, slot);
    return true;
}

void screen_pulls_pulls_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) { poller_finished(&s->poller, false, -1); return; }
    if (req->ok) pulls_note_success(s->project.repo);
    if (!req->ok) {
        // A server from before `fresh` refuses the argument it does not know.
        if (req->error.kind == API_HTTP && req->error.status == 400 && !json_is_null(json_get(req->args, "fresh"))) { if (!screen_pulls_pulls_read(s->project.repo, false, s, screen_pulls_pulls_done, &s->req)) poller_finished(&s->poller, false, -1); return; }
        pulls_note_failure(s->project.repo, &req->error, time(NULL));
        request_error_into(&s->error, req);
        s->loaded = true;
    } else if (s->fresh_pending) {
        // A write or refresh requested a newer snapshot while this read was in flight.
        if (screen_pulls_pulls_read(s->project.repo, true, s, screen_pulls_pulls_done, &s->req)) { s->fresh_pending = false; return; }
    } else {
        pulls_show(s, req->result, false);
        set_string(&s->error, NULL);
        char *key = xstrfmt("pulls:%s", s->project.repo);
        cache_store(g_store.cache, s->board, key);
        free(key);
        status_next(s);
    }
    if (!s->list_retry_read) poller_finished(&s->poller, false, -1);
    s->list_retry_read = false;
    screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void board_actions_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (!req->ok) return;
    json_free(s->catalog); s->catalog = json_clone(json_get(req->result, "actions"));
    cache_store(g_store.cache, req->result, "actions");
    pane_relayout(s->base.pane);
}

static void runs_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = all; s->run_count = n;
    pane_relayout(s->base.pane);
}

size_t screen_pulls_runs_on(PullsScreen *s, int number) { size_t n = 0; for (size_t i = 0; i < s->run_count; i++) if (session_pull_number(&s->runs[i]) == number) n++; return n; }

bool screen_pulls_run_active_on(PullsScreen *s, int number) { for (size_t i = 0; i < s->run_count; i++) if (session_pull_number(&s->runs[i]) == number && session_is_active(&s->runs[i])) return true; return false; }

void screen_pulls_pulls_load(PullsScreen *s, bool fresh) {
    if (!s->loaded) {
        char *key = xstrfmt("pulls:%s", s->project.repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { pulls_show(s, saved, true); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
        if (!json_count(s->catalog)) { Json *acts = cache_value(g_store.cache, "actions"); if (acts) { json_free(s->catalog); s->catalog = json_clone(json_get(acts, "actions")); json_free(acts); } }
    }
    s->fresh_pending = s->fresh_pending || fresh;
    if (screen_pulls_pulls_read(s->project.repo, s->fresh_pending, s, screen_pulls_pulls_done, &s->req)) s->fresh_pending = false;
    else poller_finished(&s->poller, false, -1);
    screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo);
    if (store_can_manage() && store_supports("actions") && !s->req_actions) store_call("actions", json_object(), 0, s, board_actions_done, 0, &s->req_actions);
    if (store_supports("sessions") && !s->req_runs) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, runs_done, 0, &s->req_runs); }
}

static void board_start_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        set_string(&s->write_error, NULL);
        // The list stays shown; the new session joins the runs counted on its pull request.
        if (store_supports("sessions")) { request_cancel(&s->req_runs); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, runs_done, 0, &s->req_runs); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}

void screen_pulls_board_start(PullsScreen *s, int number, const char *branch, const BoardAction *action, const char *input) {
    if (s->busy || s->uncertain || str_empty(branch)) return;
    s->busy = true; s->starting_number = number; set_string(&s->starting_id, action->id);
    char *op = board_action_operation(action);
    Json *args = board_action_arguments(action, s->project.repo, number, branch, input);
    store_call(op, args, board_action_timeout_ms(action), s, board_start_done, 0, &s->req_start);
    free(op);
    pane_relayout(s->base.pane);
}

// Merging from the board: the row has no head commit, and the server merges only the head that was read, so the
// pull request is read first and merged at the head that read returns.
static void board_merge_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    s->merging_number = 0;
    if (!req->ok) {
        char *t = request_error_text(req);
        if (!request_outcome_unknown(req)) set_string(&s->write_error, t);
        else { char *m = xstrfmt("%s The merge may still have completed; refresh before trying again.", t); set_string(&s->write_error, m); free(m); }
        free(t);
    } else {
        set_string(&s->write_error, NULL);
        const char *status = json_str(json_get(req->result, "status"));
        if (str_eq(status, "pending")) set_string(&s->write_error, "GitHub accepted the merge and is still finishing it; the pull request leaves the list once it lands.");
    }
    screen_pulls_pulls_load(s, true);
    pane_relayout(s->base.pane);
}

static void board_merge_read_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    const Json *pr = json_get(req->result, "pr");
    const char *head = json_str(json_get(pr, "headSha")), *base_ref = json_str(json_get(pr, "baseRef"));
    if (!req->ok || !head || !base_ref || !str_eq(json_str(json_get(pr, "state")), "open")) {
        if (!req->ok) request_error_into(&s->write_error, req);
        else set_string(&s->write_error, str_eq(json_str(json_get(pr, "state")), "open") ? "The server did not say which commit to merge." : "This pull request is no longer open.");
        s->merging_number = 0;
        pane_relayout(s->base.pane);
        return;
    }
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->merging_number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref); json_set_str(args, "method", "squash");
    store_call("merge_pull", args, 0, s, board_merge_done, 0, &s->req_merge);
}

void screen_pulls_board_merge(PullsScreen *s, size_t index) {
    if (index >= s->pull_count || s->merging_number || s->busy) return;
    const PullSummary *pull = &s->pulls[index];
    int number = pull->number;
    Str msg; str_init(&msg);
    str_appendf(&msg, "\xE2\x80\x9C%s\xE2\x80\x9D is squash-merged into %s on GitHub.", pull->title ? pull->title : "", str_empty(pull->base_branch) ? "its base branch" : pull->base_branch);
    if (pull_conflicting(pull)) str_appendz(&msg, "\n\nThis branch has conflicts that must be resolved before it can merge.");
    if (pull_checks_failed(pull)) str_appendz(&msg, "\n\nSome checks failed.");
    else if (str_eq(pull->checks, "pending") || str_eq(pull->checks, "expected")) str_appendz(&msg, "\n\nSome checks are still running.");
    char *title = xstrfmt("Merge #%d?", number);
    s->dialog_open = true;
    bool ok = app_confirm(title, msg.data, "Merge", true);
    s->dialog_open = false;
    free(title); str_free(&msg);
    if (!ok || s->merging_number) return;
    s->merging_number = number; set_string(&s->write_error, NULL);
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", number);
    store_call("pull", args, 0, s, board_merge_read_done, 0, &s->req_merge);
    pane_relayout(s->base.pane);
}
