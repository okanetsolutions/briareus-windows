#include "global_pulls.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

void global_pulls_forget(GlobalPullProject *p) {
    json_free(p->board); p->board = NULL;
    pull_summaries_free(p->pulls, p->count); p->pulls = NULL; p->count = 0;
    issue_summaries_free(p->issues, p->issue_count); p->issues = NULL; p->issue_count = 0;
    free(p->issues_error); p->issues_error = NULL; p->issues_truncated = false;
    p->loaded = p->truncated = false; p->synced_at = 0;
}
void global_pulls_free(GlobalPulls *s) {
    for (size_t i = 0; i < s->count; i++) { global_pulls_forget(&s->projects[i]); project_free(&s->projects[i].project); free(s->projects[i].error); }
    free(s->projects); memset(s, 0, sizeof *s);
}
void global_pulls_replace(GlobalPulls *s, const Project *projects, size_t count) {
    GlobalPulls next = {0}; next.projects = xcalloc(count, sizeof *next.projects);
    for (size_t i = 0; i < count; i++) {
        bool seen = false;
        for (size_t k = 0; k < next.count; k++) if (str_ieq(next.projects[k].project.repo, projects[i].repo)) seen = true;
        if (seen) continue;
        GlobalPullProject *p = &next.projects[next.count++];
        for (size_t k = 0; k < s->count; k++) if (str_ieq(s->projects[k].project.repo, projects[i].repo)) {
            *p = s->projects[k]; memset(&s->projects[k], 0, sizeof *p); project_free(&p->project); break;
        }
        project_copy(&p->project, &projects[i]);
    }
    global_pulls_free(s); *s = next;
}
bool global_pulls_adopt(GlobalPullProject *p, const Json *board) {
    const Json *array = json_get(board, "pulls");
    if (!json_is_array(array)) return false;
    const char *repo = json_str_nonempty(json_get(board, "repo"));
    if (repo && !str_ieq(repo, p->project.repo)) return false;
    GlobalPullProject next = {0}; next.board = json_object();
    // The overview retains bounded PR rows and only the metadata needed to open their stacks.
    static const char *keys[] = {"repo", "stacks", "author", "syncedAt", "_receivedAt"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) json_object_set(next.board, keys[i], json_clone(json_get(board, keys[i])));
    next.pulls = xcalloc(GLOBAL_PULLS_ROW_LIMIT, sizeof *next.pulls);
    for (size_t i = 0; i < json_count(array); i++) {
        PullSummary pull;
        if (!pull_summary_parse(json_at(array, i), &pull)) continue;
        bool seen = pulls_find(next.pulls, next.count, pull.number) != NULL;
        if (seen || next.count == GLOBAL_PULLS_ROW_LIMIT) {
            if (!seen) next.truncated = true;
            pull_summary_free(&pull); continue;
        }
        next.pulls[next.count++] = pull;
    }
    const Json *issues = json_get(board, "issues");
    next.issues = xcalloc(GLOBAL_PULLS_ROW_LIMIT, sizeof *next.issues);
    for (size_t i = 0; i < json_count(issues); i++) {
        IssueSummary issue;
        if (!issue_summary_parse(json_at(issues, i), &issue)) continue;
        bool seen = issues_find(next.issues, next.issue_count, issue.number) != NULL;
        if (seen || next.issue_count == GLOBAL_PULLS_ROW_LIMIT) {
            if (!seen) next.issues_truncated = true;
            issue_summary_free(&issue); continue;
        }
        next.issues[next.issue_count++] = issue;
    }
    next.issues_error = xstrdup(json_str_nonempty(json_get(board, "issuesError")));
    if (!json_is_array(issues) && !next.issues_error) next.issues_error = xstrdup("The server did not include an issues list; issue scope is unavailable.");
    next.issues_truncated |= json_bool_is(json_get(board, "issuesTruncated"), true);
    // A permission refusal must not expose even a partial issue array.
    if (next.issues_error) { issue_summaries_free(next.issues, next.issue_count); next.issues = NULL; next.issue_count = 0; }
    global_pulls_forget(p); p->issues = next.issues; p->issue_count = next.issue_count;
    p->issues_error = next.issues_error; p->issues_truncated = next.issues_truncated;
    p->board = next.board; p->pulls = next.pulls; p->count = next.count;
    p->truncated = next.truncated || json_bool_is(json_get(board, "pullsTruncated"), true);
    p->loaded = true; free(p->error); p->error = NULL; return true;
}
bool global_pull_stack_passes(const PullSummary *p, bool hide) {
    if (!hide) return true;
    StackPosition stack;
    if (!stack_position_parse(json_get(p->raw, "stack"), NULL, &stack)) return true;
    bool pass = stack.position <= 1; stack_position_free(&stack); return pass;
}
typedef struct { GlobalPullRow row; bool has_updated; time_t updated_at; int number; const char *repo; } SortedRow;
static int compare(const void *a, const void *b) {
    const SortedRow *l = a, *r = b;
    if (l->has_updated != r->has_updated) return l->has_updated ? -1 : 1;
    if (l->updated_at != r->updated_at) return l->updated_at > r->updated_at ? -1 : 1;
    int repo = _stricmp(l->repo, r->repo);
    if (repo) return repo;
    return (l->number < r->number) - (l->number > r->number);
}
BoardRow global_board_row(const GlobalPulls *s, GlobalPullRow row, GlobalBoardTab tab) {
    const GlobalPullProject *p = &s->projects[row.project];
    return tab == GLOBAL_TAB_ISSUES ? issue_board_row(&p->issues[row.pull]) : pull_board_row(&p->pulls[row.pull]);
}
GlobalPullRow *global_board_rows(const GlobalPulls *s, size_t limit, GlobalBoardTab tab, const BoardFilter *filter, bool hide, const char *login, size_t *count) {
    if (limit > s->count) limit = s->count;
    size_t total = 0;
    for (size_t i = 0; i < limit; i++) total += tab == GLOBAL_TAB_ISSUES ? s->projects[i].issue_count : s->projects[i].count;
    SortedRow *sorted = xcalloc(total, sizeof *sorted); size_t n = 0;
    for (size_t i = 0; i < limit; i++) {
        const GlobalPullProject *project = &s->projects[i];
        size_t rn = tab == GLOBAL_TAB_ISSUES ? project->issue_count : project->count;
        size_t *review = NULL;
        if (tab == GLOBAL_TAB_REVIEW) {
            const char *me = str_empty(login) ? json_str_nonempty(json_get(project->board, "author")) : login;
            review = pulls_review_list(project->pulls, project->count, json_get(project->board, "stacks"), me, &rn);
        }
        for (size_t k = 0; k < rn; k++) {
            size_t index = review ? review[k] : k;
            GlobalPullRow ref = {i, index}; BoardRow row = global_board_row(s, ref, tab);
            if (tab != GLOBAL_TAB_REVIEW && filter && !board_filter_passes(filter, &row, -1)) continue;
            if (tab == GLOBAL_TAB_PULLS && !global_pull_stack_passes(&project->pulls[index], hide)) continue;
            SortedRow next = {0}; next.row = ref; next.repo = project->project.repo;
            if (tab == GLOBAL_TAB_ISSUES) {
                const IssueSummary *issue = &project->issues[index];
                next.has_updated = issue->has_updated; next.updated_at = issue->updated_at; next.number = issue->number;
            } else {
                const PullSummary *pull = &project->pulls[index];
                next.has_updated = pull->has_updated; next.updated_at = pull->updated_at; next.number = pull->number;
            }
            sorted[n++] = next;
        }
        free(review);
    }
    qsort(sorted, n, sizeof *sorted, compare);
    GlobalPullRow *rows = xcalloc(n, sizeof *rows);
    for (size_t i = 0; i < n; i++) rows[i] = sorted[i].row;
    free(sorted); *count = n; return rows;
}
GlobalPullRow *global_pulls_rows(const GlobalPulls *s, size_t limit, const BoardFilter *filter, bool hide, size_t *count) {
    return global_board_rows(s, limit, GLOBAL_TAB_PULLS, filter, hide, NULL, count);
}
