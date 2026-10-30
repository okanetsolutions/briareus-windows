#include "global_pulls.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

void global_pulls_forget(GlobalPullProject *p) {
    json_free(p->board); p->board = NULL;
    pull_summaries_free(p->pulls, p->count); p->pulls = NULL; p->count = 0;
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
    global_pulls_forget(p); p->board = next.board; p->pulls = next.pulls; p->count = next.count;
    p->truncated = next.truncated || json_bool_is(json_get(board, "pullsTruncated"), true);
    p->loaded = true; free(p->error); p->error = NULL; return true;
}
bool global_pull_stack_passes(const PullSummary *p, bool hide) {
    if (!hide) return true;
    StackPosition stack;
    if (!stack_position_parse(json_get(p->raw, "stack"), NULL, &stack)) return true;
    bool pass = stack.position <= 1; stack_position_free(&stack); return pass;
}
typedef struct { GlobalPullRow row; const PullSummary *pull; const char *repo; } SortedRow;
static int compare(const void *a, const void *b) {
    const SortedRow *l = a, *r = b;
    if (l->pull->has_updated != r->pull->has_updated) return l->pull->has_updated ? -1 : 1;
    if (l->pull->updated_at != r->pull->updated_at) return l->pull->updated_at > r->pull->updated_at ? -1 : 1;
    int repo = _stricmp(l->repo, r->repo);
    if (repo) return repo;
    return (l->pull->number < r->pull->number) - (l->pull->number > r->pull->number);
}
GlobalPullRow *global_pulls_rows(const GlobalPulls *s, size_t limit, const BoardFilter *filter, bool hide, size_t *count) {
    if (limit > s->count) limit = s->count;
    size_t total = 0;
    for (size_t i = 0; i < limit; i++) total += s->projects[i].count;
    SortedRow *sorted = xcalloc(total, sizeof *sorted); size_t n = 0;
    for (size_t i = 0; i < limit; i++) for (size_t k = 0; k < s->projects[i].count; k++) {
        const PullSummary *p = &s->projects[i].pulls[k]; BoardRow row = pull_board_row(p);
        if (filter && !board_filter_passes(filter, &row, -1)) continue;
        if (!global_pull_stack_passes(p, hide)) continue;
        sorted[n++] = (SortedRow){{i, k}, p, s->projects[i].project.repo};
    }
    qsort(sorted, n, sizeof *sorted, compare);
    GlobalPullRow *rows = xcalloc(n, sizeof *rows);
    for (size_t i = 0; i < n; i++) rows[i] = sorted[i].row;
    free(sorted); *count = n; return rows;
}
