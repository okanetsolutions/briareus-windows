#include "screen_pulls_internal.h"

static void rebuild_actions(PullScreen *s);
static void pull_finish_poll(PullScreen *s);
static void body_done(void *owner, Request *req);
static void conv_done(void *owner, Request *req);
static void conv_page(PullScreen *s, int k, int page);
static void pull_done(void *owner, Request *req);
static void rows_done(void *owner, Request *req);
static void actions_done(void *owner, Request *req);

const PullSummary *screen_pulls_board_row(PullScreen *s) { return s->row_read ? (s->has_row ? &s->row : NULL) : (s->has_row ? &s->row : (s->has_summary ? &s->summary : NULL)); }

bool screen_pulls_is_open(PullScreen *s) { return json_is_null(s->pr) ? screen_pulls_board_row(s) != NULL : str_eq(json_str(json_get(s->pr, "state")), "open"); }

void screen_pulls_save_pull(PullScreen *s) {
    if (json_is_null(s->pr)) return;
    Json *saved = json_object();
    json_object_set(saved, "pr", json_clone(s->pr));
    json_object_set(saved, "findings", json_clone(s->findings));
    const PullSummary *row = screen_pulls_board_row(s);
    json_object_set(saved, "row", row ? json_clone(row->raw) : json_null());
    json_object_set(saved, "stack", s->has_stack ? stack_position_json(&s->stack) : json_null());
    if (s->body) json_set_str(saved, "body", s->body);
    if (s->body_author) json_set_str(saved, "bodyAuthor", s->body_author);
    char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
    cache_store(g_store.cache, saved, key);
    json_free(saved); free(key);
}

static void rebuild_actions(PullScreen *s) {
    board_actions_free(s->actions, s->action_count); s->actions = NULL; s->action_count = 0;
    if (!screen_pulls_is_open(s) || !json_str(json_get(s->pr, "headRef"))) return;
    s->actions = screen_pulls_row_actions(s->catalog, screen_pulls_board_row(s), json_int_or(json_get(json_get(s->pr, "checks"), "failed"), 0), &s->action_count);
    // ▶ Run is the Run tab here.
    for (size_t i = 0; i < s->action_count; i++) {
        if (!str_eq(s->actions[i].id, "run")) continue;
        board_action_free(&s->actions[i]);
        memmove(&s->actions[i], &s->actions[i + 1], (s->action_count - i - 1) * sizeof *s->actions);
        s->action_count--;
        break;
    }
}

static void pull_finish_poll(PullScreen *s) {
    if (s->req_pull || s->req_findings) return;
    rebuild_actions(s);
    screen_pulls_save_pull(s);
    poller_finished(&s->poller, s->error != NULL, -1);
    pane_relayout(s->base.pane);
}

static void body_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->body_read = true;
    if (req->ok) {
        const Json *pr = json_get(req->result, "pr");
        const char *body = json_str(json_get(pr, "body"));
        set_string(&s->body, body ? body : "");
        if (json_str(json_get(pr, "author"))) set_string(&s->body_author, json_str(json_get(pr, "author")));
        screen_pulls_save_pull(s);
    }
    pane_relayout(s->base.pane);
}

static void conv_done(void *owner, Request *req) {
    PullScreen *s = owner;
    int k = 0;
    while (k < CONV_FEEDS && !str_eq(req->operation, screen_pulls_conv_feeds[k].op)) k++;
    if (k == CONV_FEEDS) return;
    ConvFeed *f = &s->conv[k];
    if (!req->ok) {
        request_error_into(&f->error, req);
        json_free(f->incoming); f->incoming = NULL; f->read = true;
        pane_relayout(s->base.pane);
        return;
    }
    const Json *rows = json_get(req->result, screen_pulls_conv_feeds[k].field);
    for (size_t i = 0; i < json_count(rows); i++) json_array_push(f->incoming, json_clone(json_at(rows, i)));
    double next;
    if (json_num(json_get(req->result, "nextPage"), &next) && next > json_int_or(json_get(req->args, "page"), 1)) { conv_page(s, k, (int)next); return; }
    json_free(f->items); f->items = f->incoming; f->incoming = NULL;
    f->read = true; set_string(&f->error, NULL);
    pane_relayout(s->base.pane);
}

static void conv_page(PullScreen *s, int k, int page) {
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_num(args, "page", page);
    store_call(screen_pulls_conv_feeds[k].op, args, 0, s, conv_done, TAG_CONV, &s->conv[k].req);
}

/// Reads the comments, reviews and line comments afresh; what was read stays shown until the new lists are in.
void screen_pulls_conv_load(PullScreen *s) {
    for (int k = 0; k < CONV_FEEDS; k++) {
        ConvFeed *f = &s->conv[k];
        if (f->req || !store_supports(screen_pulls_conv_feeds[k].op)) continue;
        json_free(f->incoming); f->incoming = json_array();
        conv_page(s, k, 1);
    }
}

void screen_pulls_conv_cancel(PullScreen *s) { for (int k = 0; k < CONV_FEEDS; k++) request_cancel(&s->conv[k].req); }

static void pull_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); pull_finish_poll(s); return; }
    json_free(s->pr); s->pr = json_clone(json_get(req->result, "pr"));
    set_string(&s->error, NULL);
    // The description is its own read, once per visit and on refresh.
    if (!json_str(json_get(s->pr, "body")) && !s->body_read && !s->req_body && store_supports("pull_description")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("pull_description", args, 0, s, body_done, TAG_BODY, &s->req_body);
    }
    screen_pulls_issue_projects_load(s);
    pull_finish_poll(s);
}

static void rows_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (req->ok) pulls_note_success(s->project.repo);
    if (!req->ok) { pulls_note_failure(s->project.repo, &req->error, time(NULL)); return; }
    // A pull request the board no longer lists has been merged or closed, and its row went with it.
    if (s->has_row) pull_summary_free(&s->row);
    s->has_row = false;
    size_t count;
    PullSummary *rows = pull_summaries_parse(json_get(req->result, "pulls"), &count);
    for (size_t i = 0; i < count; i++) {
        if (rows[i].number != s->number) continue;
        pull_summary_copy(&s->row, &rows[i]); s->has_row = true;
        // Its place in a stack, with the branches of the rows it is built on; a row no longer stacked drops the overview.
        StackPosition stack;
        bool stacked = stack_position_parse(json_get(rows[i].raw, "stack"), json_get(req->result, "stacks"), &stack);
        if (stacked) stack_position_branches(&stack, rows, count);
        if (s->has_stack) stack_position_free(&s->stack);
        s->has_stack = stacked;
        if (stacked) s->stack = stack;
        break;
    }
    pull_summaries_free(rows, count);
    s->row_read = true;
    rebuild_actions(s);
    screen_pulls_issue_projects_load(s);
    pane_relayout(s->base.pane);
}

static void actions_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation) || !req->ok) return;
    json_free(s->catalog); s->catalog = json_clone(json_get(req->result, "actions"));
    cache_store(g_store.cache, req->result, "actions");
    rebuild_actions(s);
    pane_relayout(s->base.pane);
}

void screen_pulls_sessions_done_pull(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = xcalloc(n, sizeof *s->runs); s->run_count = 0;
    for (size_t i = 0; i < n; i++) if (session_pull_number(&all[i]) == s->number) session_copy(&s->runs[s->run_count++], &all[i]);
    sessions_free(all, n);
    s->runs_read = true;
    if (s->run_pending) { s->run_pending = false; screen_pulls_run_resume(s); }
    pane_relayout(s->base.pane);
}

void screen_pulls_pull_load(PullScreen *s) {
    screen_pulls_reviews_load(s, false);
    if (json_is_null(s->pr)) {
        char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) {
            json_free(s->pr); s->pr = json_clone(json_get(saved, "pr"));
            json_free(s->findings); s->findings = json_clone(json_get(saved, "findings"));
            if (!s->body && json_str(json_get(saved, "body"))) set_string(&s->body, json_str(json_get(saved, "body")));
            if (!s->body_author && json_str(json_get(saved, "bodyAuthor"))) set_string(&s->body_author, json_str(json_get(saved, "bodyAuthor")));
            if (!s->has_row) { PullSummary row; if (pull_summary_parse(json_get(saved, "row"), &row)) { s->row = row; s->has_row = true; } }
            if (!s->has_stack) s->has_stack = stack_position_restore(json_get(saved, "stack"), &s->stack);
            json_free(saved);
        }
        free(key);
        if (!json_count(s->catalog)) { Json *acts = cache_value(g_store.cache, "actions"); if (acts) { json_free(s->catalog); s->catalog = json_clone(json_get(acts, "actions")); json_free(acts); } }
        rebuild_actions(s);
        pane_relayout(s->base.pane);
    }
    if (s->req_pull) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    store_call("pull", args, 0, s, pull_done, TAG_PULL, &s->req_pull);
    // Invalidate pre-refresh rows even when a cooldown delays the replacement read.
    // Write completions call this same path to reconcile labels and assignees.
    request_cancel(&s->req_rows);
    // What the board knows about this pull request beyond its own details.
    screen_pulls_pulls_read(s->project.repo, false, s, rows_done, &s->req_rows);
    if (store_can_manage() && store_supports("actions")) store_call("actions", json_object(), 0, s, actions_done, TAG_ACTIONS, &s->req_actions);
    if (store_supports("sessions")) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, screen_pulls_sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    if (s->tab == PR_TAB_CONVERSATION) screen_pulls_conv_load(s);
}
