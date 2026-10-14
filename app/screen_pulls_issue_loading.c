#include "screen_pulls_internal.h"

static void issue_timeline_done(void *owner, Request *req);
static void issue_detail_done(void *owner, Request *req);
static bool issue_run_matches(const IssueScreen *s, const Session *run);

/// Takes the board's answer: its rows, and this issue's own row when it is still there and its full read is not.
/// A saved board only fills in.
void screen_pulls_issue_board_show(IssueScreen *s, const Json *board, bool saved) {
    issue_summaries_free(s->board_issues, s->board_issue_count); s->board_issues = issue_summaries_parse(json_get(board, "issues"), &s->board_issue_count);
    pull_summaries_free(s->board_pulls, s->board_pull_count); s->board_pulls = pull_summaries_parse(json_get(board, "pulls"), &s->board_pull_count);
    const Json *rows = json_get(board, "issues");
    bool found = false;
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        double n;
        if (!json_num(json_get(row, "number"), &n) || (int)n != s->issue.number) continue;
        IssueSummary fresh;
        found = true;
        if (!saved && !s->detail && issue_summary_parse(row, &fresh)) { issue_summary_free(&s->issue); s->issue = fresh; }
        break;
    }
    // A board GitHub refused the issues of says nothing about this one.
    if (!saved) s->gone = !found && !json_is_set(json_get(board, "issuesError")) && !json_bool_is(json_get(board, "issuesTruncated"), true);
    s->board_read = true;
}

/// Takes the issue's own read: the row it shares with the board, and all the board does not carry.
void screen_pulls_issue_detail_show(IssueScreen *s, const Json *issue) {
    IssueSummary fresh;
    if (!issue_summary_parse(issue, &fresh)) return;
    if (fresh.number != s->issue.number) { issue_summary_free(&fresh); return; }
    issue_summary_free(&s->issue); s->issue = fresh;
    json_free(s->detail); s->detail = json_clone(issue);
    s->gone = false;
    screen_pulls_issue_subs_free(s);
    const Json *items = json_get(json_get(issue, "subIssues"), "items");
    s->subs = xcalloc(json_count(items) ? json_count(items) : 1, sizeof *s->subs);
    for (size_t i = 0; i < json_count(items); i++) if (board_link_parse(json_at(items, i), &s->subs[s->sub_count])) s->sub_count++;
}

void screen_pulls_issue_timeline_show(IssueScreen *s, const Json *result, bool first) {
    if (first || !s->events) { json_free(s->events); s->events = json_array(); }
    const Json *events = json_get(result, "events");
    for (size_t i = 0; i < json_count(events); i++) json_array_push(s->events, json_clone(json_at(events, i)));
    s->next_page = json_int_or(json_get(result, "nextPage"), 0);
}

char *screen_pulls_issue_cache_key(const IssueScreen *s, const char *what) { return xstrfmt("%s:%s#%d", what, s->project.repo, s->issue.number); }

static void issue_timeline_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (!req->ok) request_error_into(&s->timeline_error, req);
    else {
        bool first = json_int_or(json_get(req->args, "page"), 1) <= 1;
        screen_pulls_issue_timeline_show(s, req->result, first);
        if (first) {
            set_string(&s->timeline_seen, s->timeline_want);
            char *key = screen_pulls_issue_cache_key(s, "issue-timeline"); cache_store(g_store.cache, req->result, key); free(key);
        }
        s->timeline_read = true;
        set_string(&s->timeline_error, NULL);
    }
    pane_relayout(s->base.pane);
}

void screen_pulls_issue_timeline_load(IssueScreen *s, int page) {
    if (!store_supports("issue_timeline")) return;
    request_cancel(&s->req_timeline);
    Json *a = json_object();
    json_set_num(a, "issue", s->issue.number); json_set_str(a, "repo", s->project.repo); json_set_num(a, "page", page);
    store_call("issue_timeline", a, 0, s, issue_timeline_done, 0, &s->req_timeline);
}

static void issue_detail_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (!req->ok) request_error_into(&s->detail_error, req);
    else {
        screen_pulls_issue_detail_show(s, json_get(req->result, "issue"));
        set_string(&s->detail_error, NULL);
        char *key = screen_pulls_issue_cache_key(s, "issue"); cache_store(g_store.cache, req->result, key); free(key);
        // The timeline is read again only when the issue has moved since; the later pages read stay until then.
        const char *updated = json_str(json_get(s->detail, "updatedAt"));
        if (!s->timeline_read || !updated || !str_eq(updated, s->timeline_seen)) { set_string(&s->timeline_want, updated); screen_pulls_issue_timeline_load(s, 1); }
    }
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static bool issue_run_matches(const IssueScreen *s, const Session *run) {
    if (!str_eq(session_repo(run), s->project.repo)) return false;
    if (session_on_issue(run, s->issue.number)) return true;
    int pr = session_pull_number(run);
    for (size_t i = 0; pr && i < s->issue.pull_count; i++)
        if (s->issue.pulls[i].number == pr && !board_link_is_foreign(&s->issue.pulls[i], s->project.repo)) return true;
    return false;
}

void screen_pulls_issue_runs_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) return;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = xcalloc(n ? n : 1, sizeof *s->runs); s->run_count = 0;
    for (size_t i = 0; i < n; i++) if (issue_run_matches(s, &all[i])) session_copy(&s->runs[s->run_count++], &all[i]);
    sessions_free(all, n);
    pane_relayout(s->base.pane);
}

void screen_pulls_issue_board_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) { poller_finished(&s->poller, false, -1); return; }
    if (req->ok) pulls_note_success(s->project.repo);
    if (!req->ok) {
        if (req->error.kind == API_HTTP && req->error.status == 400 && !json_is_null(json_get(req->args, "fresh"))) { if (!screen_pulls_pulls_read(s->project.repo, false, s, screen_pulls_issue_board_done, &s->req_board)) poller_finished(&s->poller, false, -1); return; }
        pulls_note_failure(s->project.repo, &req->error, time(NULL));
        request_error_into(&s->load_error, req);
    } else if (s->fresh_pending) {
        if (screen_pulls_pulls_read(s->project.repo, true, s, screen_pulls_issue_board_done, &s->req_board)) { s->fresh_pending = false; return; }
    } else {
        screen_pulls_issue_board_show(s, req->result, false);
        set_string(&s->load_error, NULL);
        char *key = xstrfmt("pulls:%s", s->project.repo);
        Json *saved = json_clone(req->result); json_set_num(saved, "_receivedAt", (double)time(NULL));
        cache_store(g_store.cache, saved, key); json_free(saved);
        free(key);
    }
    if (!s->list_retry_read) poller_finished(&s->poller, false, -1);
    s->list_retry_read = false;
    screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

void screen_pulls_issue_load(IssueScreen *s, bool fresh) {
    s->fresh_pending = s->fresh_pending || fresh;
    if (screen_pulls_pulls_read(s->project.repo, s->fresh_pending, s, screen_pulls_issue_board_done, &s->req_board)) s->fresh_pending = false;
    else poller_finished(&s->poller, false, -1);
    screen_pulls_list_retry_arm(&s->poller, &s->list_retry_armed, s->project.repo);
    if (store_supports("issue") && !s->req_detail) {
        Json *a = json_object(); json_set_num(a, "issue", s->issue.number); json_set_str(a, "repo", s->project.repo);
        store_call("issue", a, 0, s, issue_detail_done, 0, &s->req_detail);
    }
    if (store_supports("sessions") && !s->req_runs) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, screen_pulls_issue_runs_done, 0, &s->req_runs); }
}
