#include "screen_pulls_internal.h"

static size_t linked_issues(PullScreen *s, int *out, size_t cap);
static bool issue_round_same(const PullScreen *s, const int *numbers, size_t n);
static Json *linked_issue_entry(int number, const Json *issue);
static void issue_projects_done(void *owner, Request *req);
static void issue_projects_next(PullScreen *s);

/// The issues the Development item lists that are in this repository: the board's row first, else the pull request's own.
static size_t linked_issues(PullScreen *s, int *out, size_t cap) {
    const PullSummary *row = screen_pulls_board_row(s);
    size_t n = 0;
    if (row && row->issue_count) {
        for (size_t i = 0; i < row->issue_count && n < cap; i++) if (!board_link_is_foreign(&row->issues[i], s->project.repo)) out[n++] = row->issues[i].number;
        return n;
    }
    const Json *arr = json_get(s->pr, "issues");
    for (size_t i = 0; i < json_count(arr) && n < cap; i++) {
        BoardLink l;
        if (!board_link_parse(json_at(arr, i), &l)) continue;
        if (!board_link_is_foreign(&l, s->project.repo)) out[n++] = l.number;
        board_link_free(&l);
    }
    return n;
}

static bool issue_round_same(const PullScreen *s, const int *numbers, size_t n) {
    return n == s->issue_round_count && !memcmp(numbers, s->issue_round, n * sizeof *numbers);
}

static Json *linked_issue_entry(int number, const Json *issue) {
    Json *e = json_object();
    json_set_num(e, "number", number);
    json_object_set(e, "projects", json_clone(json_get(issue, "projects")));
    if (json_str(json_get(issue, "projectsError"))) json_set_str(e, "projectsError", json_str(json_get(issue, "projectsError")));
    return e;
}

static void issue_projects_done(void *owner, Request *req) {
    PullScreen *s = owner;
    // Throttling stops the round with this issue unread, so it and the rest are read on the next poll rather
    // than hitting the limit again.
    if (req->error.kind == API_HTTP && req->error.status == 429) return;
    double number = 0; json_num(json_get(req->args, "issue"), &number);
    if (req->ok) {
        json_array_push(s->issue_incoming, linked_issue_entry((int)number, json_get(req->result, "issue")));
        char *key = screen_pulls_saved_issue_key(s->project.repo, (int)number); cache_store(g_store.cache, req->result, key); free(key);
    } else {
        // One that cannot be read keeps what was shown of it, if anything; the others still show.
        const Json *kept = NULL;
        for (size_t i = 0; i < json_count(s->issue_projects) && !kept; i++) if (json_int_or(json_get(json_at(s->issue_projects, i), "number"), 0) == (int)number) kept = json_at(s->issue_projects, i);
        json_array_push(s->issue_incoming, kept ? json_clone(kept) : json_null());
    }
    issue_projects_next(s);
}

/// Reads the next linked issue, or, once they are all in, shows what they said.
static void issue_projects_next(PullScreen *s) {
    int numbers[LINKED_ISSUES_MAX]; size_t n = linked_issues(s, numbers, LINKED_ISSUES_MAX);
    // A list that changed partway, as when the board's row replaces the saved one, starts the round again on it.
    if (!issue_round_same(s, numbers, n)) {
        memcpy(s->issue_round, numbers, n * sizeof *numbers); s->issue_round_count = n;
        json_free(s->issue_incoming); s->issue_incoming = json_array();
    }
    size_t at = json_count(s->issue_incoming);
    if (at < n) {
        Json *a = json_object(); json_set_num(a, "issue", numbers[at]); json_set_str(a, "repo", s->project.repo);
        store_call("issue", a, 0, s, issue_projects_done, TAG_ISSUE, &s->req_issue);
        return;
    }
    Json *done = json_array();
    for (size_t i = 0; i < json_count(s->issue_incoming); i++) if (!json_is_null(json_at(s->issue_incoming, i))) json_array_push(done, json_clone(json_at(s->issue_incoming, i)));
    json_free(s->issue_incoming); s->issue_incoming = NULL;
    json_free(s->issue_projects); s->issue_projects = done;
    s->issues_read = true;
    pane_relayout(s->base.pane);
}

/// The linked issues' projects: what was saved of them at once, then each issue read again.
void screen_pulls_issue_projects_load(PullScreen *s) {
    if (s->req_issue || !store_supports("issue")) return;
    int numbers[LINKED_ISSUES_MAX]; size_t n = linked_issues(s, numbers, LINKED_ISSUES_MAX);
    // Read once per visit, unless the issues it links have changed since.
    if (s->issues_read && issue_round_same(s, numbers, n)) return;
    s->issues_read = false;
    // A pull request that no longer links any issue drops the projects it showed.
    if (!n) { if (s->issue_projects) { json_free(s->issue_projects); s->issue_projects = NULL; pane_relayout(s->base.pane); } return; }
    if (!s->issue_projects) {
        Json *saved_all = json_array();
        for (size_t i = 0; i < n; i++) {
            char *key = screen_pulls_saved_issue_key(s->project.repo, numbers[i]); Json *saved = cache_value(g_store.cache, key); free(key);
            if (saved) { json_array_push(saved_all, linked_issue_entry(numbers[i], json_get(saved, "issue"))); json_free(saved); }
        }
        if (json_count(saved_all)) s->issue_projects = saved_all; else json_free(saved_all);
    }
    // A round throttled partway carries on where it stopped.
    if (!s->issue_incoming) s->issue_incoming = json_array();
    issue_projects_next(s);
}
