#include "issue_pages.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

void issue_pages_clear(IssuePages *p) {
    issue_summaries_free(p->issues, p->count);
    free(p->error);
    memset(p, 0, sizeof *p);
}

bool board_lists_every_issue(const Json *board) {
    return !json_is_set(json_get(board, "issuesError")) && !json_bool_is(json_get(board, "issuesTruncated"), true);
}

bool issue_pages_offered(const Json *board) {
    return json_bool_is(json_get(board, "issuesTruncated"), true) && !json_is_set(json_get(board, "issuesError"));
}

bool issue_pages_can_load_more(const IssuePages *p, const Json *board, bool routed) {
    return routed && issue_pages_offered(board) && !p->ended;
}

static bool held(const IssuePages *p, int number) {
    for (size_t i = 0; i < p->count; i++) if (p->issues[i].number == number) return true;
    return false;
}

static bool board_row_lists(const Json *board, int number) {
    const Json *rows = json_get(board, "issues");
    for (size_t i = 0; i < json_count(rows); i++) if (json_int_or(json_get(json_at(rows, i), "number"), 0) == number) return true;
    return false;
}

bool issue_pages_cut_short(const IssuePages *p, const Json *board, bool routed) {
    if (!issue_pages_offered(board)) return false;
    if (!routed) return true;
    if (!p->ended) return false;
    if (p->mirror_cut) return true;
    for (size_t i = 0; i < p->count; i++) if (!board_row_lists(board, p->issues[i].number)) return false;
    return true;
}

bool issue_pages_retries(const IssuePages *p) { return p->error != NULL; }

const char *issue_pages_retry_error(const IssuePages *p, const Json *board, bool routed) {
    return issue_pages_can_load_more(p, board, routed) ? p->error : NULL;
}

Json *issue_pages_query(const IssuePages *p, const char *repo, const Json *board) {
    int listed = (int)json_count(json_get(board, "issues"));
    int from = p->answered && p->next_offset > listed ? p->next_offset : listed;
    int offset = from > ISSUE_PAGES_OVERLAP ? from - ISSUE_PAGES_OVERLAP : 0;
    Json *q = json_object();
    json_set_str(q, "repo", repo); json_set_num(q, "offset", offset); json_set_num(q, "limit", ISSUE_PAGES_SIZE);
    return q;
}

void issue_pages_reading(IssuePages *p) { free(p->error); p->error = NULL; }

bool issue_pages_receive(IssuePages *p, const Json *page) {
    const Json *rows = json_get(page, "issues"), *next = json_get(page, "nextOffset");
    int offset = json_int_or(next, -1);
    if (!json_is_array(rows)) return false;
    if (!json_is_null(next) && offset < 0) return false;
    p->issues = xrealloc(p->issues, (p->count + json_count(rows) + 1) * sizeof *p->issues);
    for (size_t i = 0; i < json_count(rows); i++) {
        IssueSummary issue;
        if (!issue_summary_parse(json_at(rows, i), &issue)) continue;
        if (held(p, issue.number)) { issue_summary_free(&issue); continue; }
        p->issues[p->count++] = issue;
    }
    p->answered = true;
    p->ended = json_is_null(next);
    p->next_offset = p->ended ? 0 : offset;
    p->mirror_cut = json_bool_is(json_get(page, "issuesTruncated"), true);
    issue_pages_reading(p);
    return true;
}

void issue_pages_fail(IssuePages *p, const char *error) {
    free(p->error);
    p->error = xstrdup(error ? error : "");
}

void issue_pages_drop(IssuePages *p, int number) {
    for (size_t i = 0; i < p->count; i++) {
        if (p->issues[i].number != number) continue;
        issue_summary_free(&p->issues[i]);
        memmove(&p->issues[i], &p->issues[i + 1], (p->count - i - 1) * sizeof *p->issues);
        p->count--;
        return;
    }
}

IssueSummary *issue_pages_listed(const IssuePages *p, const Json *board, const IssueSummary *board_issues, size_t board_count, size_t *count) {
    bool extra = issue_pages_offered(board);
    IssueSummary *out = xcalloc(board_count + (extra ? p->count : 0) + 1, sizeof *out);
    size_t n = 0;
    for (size_t i = 0; i < board_count; i++) out[n++] = board_issues[i];
    for (size_t i = 0; extra && i < p->count; i++) if (!issues_find(board_issues, board_count, p->issues[i].number)) out[n++] = p->issues[i];
    *count = n;
    return out;
}
