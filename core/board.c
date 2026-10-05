#include "board.h"
#include "str.h"
#include "models.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/// Logins and label names are compared folded: GitHub hands the same person back in either case.
static bool fold_eq(const char *a, const char *b) { return str_ieq(a ? a : "", b ? b : ""); }
static DEFINE_LIST_PARSE(PullLabel, labels_parse, pull_label_parse)
static DEFINE_LIST_FREE(PullLabel, labels_free, pull_label_free)
static PullLabel *labels_copy(const PullLabel *labels, size_t count) {
    PullLabel *out = xcalloc(count, sizeof *out);
    for (size_t i = 0; i < count; i++) { out[i].name = xstrdup(labels[i].name); out[i].color = xstrdup(labels[i].color); }
    return out;
}
static DEFINE_LIST_PARSE(BoardLink, links_parse, board_link_parse)
static DEFINE_LIST_FREE(BoardLink, links_free, board_link_free)

// MARK: - Labels and links

bool pull_label_parse(const Json *value, PullLabel *out) {
    memset(out, 0, sizeof *out);
    const char *name = json_str(json_get(value, "name"));
    if (!name) name = json_str(value);
    if (str_empty(name)) return false;
    out->name = xstrdup(name); out->color = json_dup_str(json_get(value, "color"));
    return true;
}
void pull_label_free(PullLabel *l) { if (!l) return; free(l->name); free(l->color); memset(l, 0, sizeof *l); }
bool pull_label_rgb(const PullLabel *l, int rgb[3]) {
    if (!l->color || strlen(l->color) != 6) return false;
    unsigned v = 0;
    for (int i = 0; i < 6; i++) {
        char c = l->color[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10; else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return false;
        v = v * 16 + (unsigned)d;
    }
    rgb[0] = (int)((v >> 16) & 0xFF); rgb[1] = (int)((v >> 8) & 0xFF); rgb[2] = (int)(v & 0xFF);
    return true;
}

bool board_link_parse(const Json *value, BoardLink *out) {
    memset(out, 0, sizeof *out);
    double number;
    if (!json_num(json_get(value, "number"), &number)) return false;
    out->number = (int)number;
    const char *title = json_str(json_get(value, "title"));
    out->title = title ? xstrdup(title) : xstrfmt("#%d", out->number);
    out->url = json_dup_str(json_get(value, "url")); out->repo = json_dup_str(json_get(value, "repo"));
    out->draft = json_bool_is(json_get(value, "draft"), true);
    out->state = json_dup_str(json_get(value, "state")); out->state_reason = json_dup_str(json_get(value, "stateReason"));
    out->labels = labels_parse(json_get(value, "labels"), &out->label_count);
    return true;
}
void board_link_free(BoardLink *l) {
    if (!l) return;
    free(l->title); free(l->url); free(l->repo); free(l->state); free(l->state_reason); labels_free(l->labels, l->label_count);
    memset(l, 0, sizeof *l);
}
void board_link_copy(BoardLink *into, const BoardLink *from) {
    memset(into, 0, sizeof *into);
    into->number = from->number; into->title = xstrdup(from->title);
    into->url = xstrdup(from->url); into->repo = xstrdup(from->repo);
    into->draft = from->draft; into->state = xstrdup(from->state); into->state_reason = xstrdup(from->state_reason);
    into->labels = labels_copy(from->labels, from->label_count); into->label_count = from->label_count;
}
bool board_link_is_foreign(const BoardLink *l, const char *repo) { return l->repo ? !fold_eq(l->repo, repo) : false; }
char *board_link_reference(const BoardLink *l, const char *repo) {
    return board_link_is_foreign(l, repo) ? xstrfmt("%s#%d", l->repo ? l->repo : "", l->number) : xstrfmt("#%d", l->number);
}
bool board_link_not_planned(const BoardLink *l) { return str_eq(l->state, "closed") && str_eq(l->state_reason, "not_planned"); }

// MARK: - Pull requests

bool pull_summary_parse(const Json *value, PullSummary *out) {
    memset(out, 0, sizeof *out);
    double number;
    if (!json_num(json_get(value, "number"), &number) || number < 1) return false;
    out->number = (int)number; out->raw = json_clone(value);
    const char *title = json_str(json_get(value, "title"));
    out->title = title ? xstrdup(title) : xstrfmt("Pull request #%d", out->number);
    out->url = json_dup_str(json_get(value, "url"));
    out->branch = xstrdup(json_str_or(json_get(value, "branch"), ""));
    out->base_branch = xstrdup(json_str_or(json_get(value, "baseBranch"), ""));
    out->draft = json_bool_is(json_get(value, "draft"), true);
    out->author = json_dup_str(json_get(value, "author"));
    out->assignees = json_dup_strings(json_get(value, "assignees"), &out->assignee_count);
    const Json *reviewers = json_get(value, "reviewers");
    size_t rn = json_count(reviewers);
    out->reviewers = xcalloc(rn, sizeof *out->reviewers);
    for (size_t i = 0; i < rn; i++) {
        const Json *r = json_at(reviewers, i);
        const char *user = json_str(json_get(r, "user"));
        if (!user) continue;
        out->reviewers[out->reviewer_count].user = xstrdup(user);
        out->reviewers[out->reviewer_count].state = xstrdup(json_str_or(json_get(r, "state"), ""));
        out->reviewer_count++;
    }
    out->labels = labels_parse(json_get(value, "labels"), &out->label_count);
    out->issues = links_parse(json_get(value, "issues"), &out->issue_count);
    out->mergeable = xstrdup(json_str_or(json_get(value, "mergeable"), "unknown"));
    out->checks = json_dup_str(json_get(value, "checks"));
    out->review_decision = json_dup_str(json_get(value, "reviewDecision"));
    out->recommended = json_dup_str(json_get(value, "recommended"));
    out->has_updated = board_date_parse(json_str(json_get(value, "updatedAt")), &out->updated_at);
    return true;
}
void pull_summary_free(PullSummary *p) {
    if (!p) return;
    free(p->title); free(p->url); free(p->branch); free(p->base_branch); free(p->author);
    str_array_free(p->assignees, p->assignee_count);
    for (size_t i = 0; i < p->reviewer_count; i++) { free(p->reviewers[i].user); free(p->reviewers[i].state); }
    free(p->reviewers);
    labels_free(p->labels, p->label_count); links_free(p->issues, p->issue_count);
    free(p->mergeable); free(p->checks); free(p->review_decision); free(p->recommended);
    json_free(p->raw);
    memset(p, 0, sizeof *p);
}
void pull_summary_copy(PullSummary *into, const PullSummary *from) { pull_summary_parse(from->raw, into); }
static bool carries(const PullLabel *labels, size_t count, const char *label) {
    for (size_t i = 0; i < count; i++) if (fold_eq(labels[i].name, label)) return true;
    return false;
}
bool pull_conflicting(const PullSummary *p) { return str_eq(p->mergeable, "conflicting"); }
bool pull_has_conflicts(const PullSummary *p) { return pull_conflicting(p) || carries(p->labels, p->label_count, "has-conflicts"); }
bool pull_checks_failed(const PullSummary *p) { return str_eq(p->checks, "failure") || str_eq(p->checks, "error"); }
bool pull_awaits_feedback(const PullSummary *p) { return carries(p->labels, p->label_count, "feedback-given"); }
DEFINE_LIST_PARSE(PullSummary, pull_summaries_parse, pull_summary_parse)
DEFINE_LIST_FREE(PullSummary, pull_summaries_free, pull_summary_free)

// MARK: - Issues

bool issue_summary_parse(const Json *value, IssueSummary *out) {
    memset(out, 0, sizeof *out);
    double number;
    if (!json_num(json_get(value, "number"), &number) || number < 1) return false;
    out->number = (int)number;
    const char *title = json_str(json_get(value, "title"));
    out->title = title ? xstrdup(title) : xstrfmt("Issue #%d", out->number);
    out->url = json_dup_str(json_get(value, "url")); out->author = json_dup_str(json_get(value, "author"));
    out->assignees = json_dup_strings(json_get(value, "assignees"), &out->assignee_count);
    out->labels = labels_parse(json_get(value, "labels"), &out->label_count);
    out->comments = json_int_or(json_get(value, "comments"), 0);
    out->milestone = json_dup_str(json_get(value, "milestone"));
    out->has_created = board_date_parse(json_str(json_get(value, "createdAt")), &out->created_at);
    out->has_updated = board_date_parse(json_str(json_get(value, "updatedAt")), &out->updated_at);
    out->has_parent = board_link_parse(json_get(value, "parent"), &out->parent);
    out->sub_issues = json_int_or(json_get(json_get(value, "subIssues"), "total"), 0);
    out->sub_issues_done = json_int_or(json_get(json_get(value, "subIssues"), "completed"), 0);
    out->pulls = links_parse(json_get(value, "pulls"), &out->pull_count);
    return true;
}
void issue_summary_free(IssueSummary *i) {
    if (!i) return;
    free(i->title); free(i->url); free(i->author); free(i->milestone);
    str_array_free(i->assignees, i->assignee_count); labels_free(i->labels, i->label_count);
    if (i->has_parent) board_link_free(&i->parent);
    links_free(i->pulls, i->pull_count);
    memset(i, 0, sizeof *i);
}
bool issue_is_epic(const IssueSummary *i) { return i->sub_issues > 0; }
DEFINE_LIST_PARSE(IssueSummary, issue_summaries_parse, issue_summary_parse)
DEFINE_LIST_FREE(IssueSummary, issue_summaries_free, issue_summary_free)

typedef struct { const IssueSummary *issues; size_t count; const char *repo; IssueRow *rows; size_t row_count; bool *drawn; } Nest;
static int parent_of(const Nest *n, const IssueSummary *issue) {
    if (!issue->has_parent || issue->parent.number == issue->number || board_link_is_foreign(&issue->parent, n->repo)) return 0;
    for (size_t i = 0; i < n->count; i++) if (n->issues[i].number == issue->parent.number) return issue->parent.number;
    return 0;
}
static void draw(Nest *n, size_t index, int depth) {
    if (n->drawn[index]) return;
    n->drawn[index] = true;
    n->rows[n->row_count].index = index; n->rows[n->row_count].depth = depth; n->row_count++;
    for (size_t i = 0; i < n->count; i++) if (parent_of(n, &n->issues[i]) == n->issues[index].number) draw(n, i, depth + 1);
}
IssueRow *issues_nested(const IssueSummary *issues, size_t count, const char *repo, size_t *row_count) {
    Nest n = { issues, count, repo, xcalloc(count, sizeof(IssueRow)), 0, xcalloc(count, sizeof(bool)) };
    for (size_t i = 0; i < count; i++) if (parent_of(&n, &issues[i]) == 0) draw(&n, i, 0);
    for (size_t i = 0; i < count; i++) draw(&n, i, 0);
    free(n.drawn);
    *row_count = n.row_count;
    return n.rows;
}
char *issue_prompt(const IssueSummary *issue, const char *repo) {
    Str s; str_init(&s);
    str_appendf(&s, "Issue #%d: %s\n\n", issue->number, issue->title);
    str_appendf(&s, "Read %s issue #%d in full before you change anything: `gh issue view %d --repo %s --comments`. Its comments usually carry decisions the description was written before.\n\n",
                repo, issue->number, issue->number, repo);
    if (issue->has_parent) {
        const char *parent_repo = board_link_is_foreign(&issue->parent, repo) ? (issue->parent.repo ? issue->parent.repo : repo) : repo;
        str_appendf(&s, "It is a sub-issue of %s#%d (%s). Read that epic too, for the shape this piece has to fit; implement only this issue.\n\n",
                    parent_repo, issue->parent.number, issue->parent.title);
    }
    str_appendf(&s, "Then implement it on this session\xE2\x80\x99s own branch, verify the change the way this repository verifies changes, and open a pull request whose body says `Closes #%d`, so merging it closes the issue.\n\n", issue->number);
    str_appendz(&s, "If the issue is too ambiguous to implement as written, say what is missing and stop rather than guessing at it.");
    return str_detach(&s);
}
const IssueSummary *issues_find(const IssueSummary *issues, size_t count, int number) {
    for (size_t i = 0; i < count; i++) if (issues[i].number == number) return &issues[i];
    return NULL;
}
size_t *issue_open_sub_issues(const IssueSummary *issues, size_t count, int epic, const char *repo, size_t *found) {
    size_t *out = xcalloc(count ? count : 1, sizeof *out);
    *found = 0;
    for (size_t i = 0; i < count; i++) {
        const IssueSummary *it = &issues[i];
        if (it->has_parent && it->parent.number == epic && it->number != epic && !board_link_is_foreign(&it->parent, repo)) out[(*found)++] = i;
    }
    return out;
}
const PullSummary *pulls_find(const PullSummary *pulls, size_t count, int number) {
    for (size_t i = 0; i < count; i++) if (pulls[i].number == number) return &pulls[i];
    return NULL;
}

// MARK: - Filters

BoardRow pull_board_row(const PullSummary *p) {
    BoardRow row = { p->author, p->reviewers, p->reviewer_count, p->assignees, p->assignee_count, p->labels, p->label_count };
    return row;
}
BoardRow issue_board_row(const IssueSummary *i) {
    BoardRow row = { i->author, NULL, 0, i->assignees, i->assignee_count, i->labels, i->label_count };
    return row;
}

const char *filter_kind_name(FilterKind kind) {
    switch (kind) {
    case FILTER_AUTHOR: return "author";
    case FILTER_REVIEWER: return "reviewer";
    case FILTER_ASSIGNEE: return "assignee";
    default: return "label";
    }
}
void board_filter_init(BoardFilter *f) { f->author = xstrdup(""); f->reviewer = xstrdup(""); f->assignee = xstrdup(""); f->label = xstrdup(""); }
void board_filter_free(BoardFilter *f) { if (!f) return; free(f->author); free(f->reviewer); free(f->assignee); free(f->label); memset(f, 0, sizeof *f); }
void board_filter_copy(BoardFilter *into, const BoardFilter *from) { into->author = xstrdup(from->author); into->reviewer = xstrdup(from->reviewer); into->assignee = xstrdup(from->assignee); into->label = xstrdup(from->label); }
bool board_filter_equal(const BoardFilter *a, const BoardFilter *b) { return str_eq(a->author, b->author) && str_eq(a->reviewer, b->reviewer) && str_eq(a->assignee, b->assignee) && str_eq(a->label, b->label); }
void board_filter_opening(BoardFilter *f, const char *author, const BoardRow *rows, size_t count) {
    board_filter_init(f);
    if (str_empty(author)) return;
    for (size_t i = 0; i < count; i++) if (fold_eq(rows[i].author, author)) { free(f->author); f->author = str_fold(author); return; }
}
bool board_filter_is_on(const BoardFilter *f) { return !(str_empty(f->author) && str_empty(f->reviewer) && str_empty(f->assignee) && str_empty(f->label)); }
static char **filter_slot(BoardFilter *f, FilterKind kind) {
    switch (kind) {
    case FILTER_AUTHOR: return &f->author;
    case FILTER_REVIEWER: return &f->reviewer;
    case FILTER_ASSIGNEE: return &f->assignee;
    default: return &f->label;
    }
}
const char *board_filter_get(const BoardFilter *f, FilterKind kind) { return *filter_slot((BoardFilter *)f, kind); }
void board_filter_set(BoardFilter *f, FilterKind kind, const char *value) {
    char **slot = filter_slot(f, kind);
    free(*slot); *slot = str_fold(value);
}
static size_t carried(FilterKind kind, const BoardRow *row, const char **out, size_t cap) {
    size_t n = 0;
    switch (kind) {
    case FILTER_AUTHOR: if (row->author && n < cap) out[n++] = row->author; break;
    case FILTER_REVIEWER: for (size_t i = 0; i < row->reviewer_count && n < cap; i++) out[n++] = row->reviewers[i].user; break;
    case FILTER_ASSIGNEE: for (size_t i = 0; i < row->assignee_count && n < cap; i++) out[n++] = row->assignees[i]; break;
    default: for (size_t i = 0; i < row->label_count && n < cap; i++) out[n++] = row->labels[i].name; break;
    }
    return n;
}
#define MAX_CARRIED 256
bool board_filter_passes(const BoardFilter *f, const BoardRow *row, int skipping) {
    for (int kind = 0; kind < FILTER_KIND_COUNT; kind++) {
        if (kind == skipping) continue;
        const char *pick = board_filter_get(f, (FilterKind)kind);
        if (str_empty(pick)) continue;
        const char *values[MAX_CARRIED]; size_t n = carried((FilterKind)kind, row, values, MAX_CARRIED);
        bool found = false;
        for (size_t i = 0; i < n && !found; i++) found = fold_eq(values[i], pick);
        if (!found) return false;
    }
    return true;
}
static int compare_options(const void *a, const void *b) {
    const FilterOption *x = a, *y = b;
    int c = _stricmp(x->text, y->text);
    return c ? c : strcmp(x->text, y->text);
}
FilterOption *board_filter_options(const BoardFilter *f, FilterKind kind, const BoardRow *rows, size_t count, size_t *option_count) {
    FilterOption *options = NULL; size_t n = 0, cap = 0;
    for (size_t r = 0; r < count; r++) {
        if (!board_filter_passes(f, &rows[r], (int)kind)) continue;
        const char *values[MAX_CARRIED]; size_t m = carried(kind, &rows[r], values, MAX_CARRIED);
        const char *seen[MAX_CARRIED]; size_t seen_count = 0;
        for (size_t i = 0; i < m; i++) {
            bool dup = false;
            for (size_t j = 0; j < seen_count && !dup; j++) dup = fold_eq(seen[j], values[i]);
            if (dup) continue;
            seen[seen_count++] = values[i];
            size_t k = 0;
            for (; k < n; k++) if (fold_eq(options[k].value, values[i])) break;
            if (k == n) {
                if (n == cap) { cap = cap ? cap * 2 : 8; options = xrealloc(options, cap * sizeof *options); }
                options[n].value = str_fold(values[i]); options[n].text = xstrdup(values[i]); options[n].count = 0; n++;
            }
            options[k].count++;
        }
    }
    const char *pick = board_filter_get(f, kind);
    if (!str_empty(pick)) {
        bool listed = false;
        for (size_t k = 0; k < n && !listed; k++) listed = str_eq(options[k].value, pick);
        if (!listed) {
            if (n == cap) { cap = cap ? cap * 2 : 8; options = xrealloc(options, cap * sizeof *options); }
            options[n].value = xstrdup(pick); options[n].text = xstrdup(pick); options[n].count = 0; n++;
        }
    }
    if (n > 1) qsort(options, n, sizeof *options, compare_options);
    *option_count = n;
    return options;
}
void filter_options_free(FilterOption *options, size_t count) {
    if (!options) return;
    for (size_t i = 0; i < count; i++) { free(options[i].value); free(options[i].text); }
    free(options);
}

// MARK: - Actions

static const BoardAction known_actions[] = {
    { "run", "Run", "Prepare this pull request in a clean workspace and serve the app from it", false, { NULL, NULL, false } },
    { "review", "Code review", "Run the provider\xE2\x80\x99s code review on this pull request and publish it", false, { NULL, NULL, false } },
    { "solve-conflicts", "Solve conflicts", "Merge the base branch in, resolve the conflicts and push the result", false, { NULL, NULL, false } },
    { "fix-checks", "Fix failing checks", "Read this pull request\xE2\x80\x99s failing CI checks, fix what the branch broke and push the fixes", false, { NULL, NULL, false } },
    { "implement-feedback", "Implement feedback", "Address the review findings on this pull request, push the fixes, and have those changes reviewed automatically", false, { NULL, NULL, false } },
    { "custom-feedback", "Give feedback", "Say in your own words what to change on this pull request, and it is implemented and pushed", true,
      { "Your feedback", "What should change on this pull request?", true } },
    { "test-sheet", "Test sheet", "Derive the manual QA checklist from this pull request\xE2\x80\x99s diff and post it as one editable comment", false, { NULL, NULL, false } },
    { "test-run", "Record QA", "Execute this pull request\xE2\x80\x99s test sheet in a fresh workspace and record a video of every scenario", false, { NULL, NULL, false } },
    { "pr-body-summary", "PR body", "Rewrite this pull request\xE2\x80\x99s description from its own diff, following the team template", false, { NULL, NULL, false } },
    { "delete-self-comments", "Delete my comments", "Remove every comment and review the configured GitHub account left on this pull request", false, { NULL, NULL, false } },
};
#define KNOWN_COUNT (sizeof known_actions / sizeof *known_actions)

const BoardAction *board_actions_known(size_t *count) { *count = KNOWN_COUNT; return known_actions; }
void board_action_free(BoardAction *a) {
    if (!a) return;
    free(a->id); free(a->label); free(a->hint); free(a->input.label); free(a->input.placeholder);
    memset(a, 0, sizeof *a);
}
void board_action_copy(BoardAction *into, const BoardAction *from) {
    into->id = xstrdup(from->id); into->label = xstrdup(from->label); into->hint = xstrdup(from->hint ? from->hint : "");
    into->has_input = from->has_input;
    into->input.label = from->has_input && from->input.label ? xstrdup(from->input.label) : NULL;
    into->input.placeholder = from->has_input && from->input.placeholder ? xstrdup(from->input.placeholder) : NULL;
    into->input.required = from->has_input && from->input.required;
}
char *board_action_operation(const BoardAction *a) {
    if (str_eq(a->id, "run")) return xstrdup("serve_pull");
    if (str_eq(a->id, "review")) return xstrdup("review");
    return xstrdup("action");
}
int board_action_timeout_ms(const BoardAction *a) { return str_eq(a->id, "run") ? 170000 : 0; }
Json *board_action_arguments(const BoardAction *a, const char *repo, int number, const char *branch, const char *input) {
    Json *args = json_object();
    json_set_str(args, "repo", repo); json_set_num(args, "prNumber", number);
    if (str_eq(a->id, "review") && branch) json_set_str(args, "branch", branch);
    else if (!str_eq(a->id, "run") && !str_eq(a->id, "review")) json_set_str(args, "action", a->id);
    if (a->has_input && input) {
        char *trimmed = str_trim(input);
        if (*trimmed) json_set_str(args, "input", trimmed);
        free(trimmed);
    }
    return args;
}
BoardAction *board_actions_offered(const Json *catalog, const PullSummary *pull, int failed_checks, size_t *count) {
    // What the server lists, by id, in its order.
    size_t served_count = json_count(catalog);
    BoardAction *served = xcalloc(served_count, sizeof *served);
    size_t sn = 0;
    for (size_t i = 0; i < served_count; i++) {
        const Json *entry = json_at(catalog, i);
        const char *id = json_str(json_get(entry, "id")), *label = json_str(json_get(entry, "label"));
        if (!id || !label) continue;
        size_t k = 0; for (; k < sn; k++) if (str_eq(served[k].id, id)) break;
        if (k < sn) board_action_free(&served[k]); else sn++;
        BoardAction *a = &served[k];
        memset(a, 0, sizeof *a);
        a->id = xstrdup(id); a->label = xstrdup(label);
        a->hint = xstrdup(json_str_or(json_get(entry, "hint"), ""));
        const char *input_label = json_str(json_get(json_get(entry, "input"), "label"));
        if (input_label) {
            a->has_input = true; a->input.label = xstrdup(input_label);
            a->input.placeholder = xstrdup(json_str_or(json_get(json_get(entry, "input"), "placeholder"), ""));
            a->input.required = json_bool_is(json_get(json_get(entry, "input"), "required"), true);
        }
    }
    BoardAction *out = xcalloc(KNOWN_COUNT + sn + 1, sizeof *out);
    size_t n = 0;
    for (size_t i = 0; i < KNOWN_COUNT; i++) {
        const BoardAction *k = &known_actions[i];
        const BoardAction *s = NULL;
        for (size_t j = 0; j < sn; j++) if (str_eq(served[j].id, k->id)) { s = &served[j]; break; }
        bool offered;
        // Run and Review have routes of their own; the rest are errands the server has to list.
        if (sn && !s && !str_eq(k->id, "run") && !str_eq(k->id, "review")) offered = false;
        else if (str_eq(k->id, "solve-conflicts")) offered = pull ? pull_has_conflicts(pull) : true;
        else if (str_eq(k->id, "fix-checks")) offered = failed_checks > 0 || (pull ? pull_checks_failed(pull) : false);
        else if (str_eq(k->id, "implement-feedback")) offered = pull ? pull_awaits_feedback(pull) : true;
        else offered = true;
        if (!offered) continue;
        BoardAction *a = &out[n++];
        a->id = xstrdup(k->id); a->label = xstrdup(k->label); a->hint = xstrdup(k->hint);
        // The server words the question and adds errands this app predates.
        const BoardAction *input_source = s && s->has_input ? s : k;
        a->has_input = input_source->has_input;
        if (a->has_input) {
            a->input.label = xstrdup(input_source->input.label ? input_source->input.label : "");
            a->input.placeholder = xstrdup(input_source->input.placeholder ? input_source->input.placeholder : "");
            a->input.required = input_source->input.required;
        }
    }
    // Errands this app no longer offers, even when the server still lists them.
    static const char *const dropped[] = { "qa" };
    for (size_t j = 0; j < sn; j++) {
        bool known = false;
        for (size_t i = 0; i < KNOWN_COUNT && !known; i++) known = str_eq(known_actions[i].id, served[j].id);
        for (size_t i = 0; i < sizeof dropped / sizeof *dropped && !known; i++) known = str_eq(dropped[i], served[j].id);
        if (known) continue;
        board_action_copy(&out[n++], &served[j]);
    }
    for (size_t j = 0; j < sn; j++) board_action_free(&served[j]);
    free(served);
    *count = n;
    return out;
}
DEFINE_LIST_FREE(BoardAction, board_actions_free, board_action_free)

// MARK: - Merge

char **merge_warnings(const Json *mergeable, const char *state, size_t *count) {
    char **notes = xmalloc(5 * sizeof *notes); size_t n = 0;
    if (json_bool_is(mergeable, false)) notes[n++] = xstrdup("This branch has conflicts that must be resolved before it can merge.");
    if (json_is_null(mergeable)) notes[n++] = xstrdup("GitHub is still checking whether this branch can merge.");
    if (str_eq(state, "blocked")) notes[n++] = xstrdup("GitHub reports this pull request as blocked: a required review or check is missing.");
    else if (str_eq(state, "behind")) notes[n++] = xstrdup("This branch is behind its base branch and may need updating before it can merge.");
    notes[n] = NULL;
    *count = n;
    return notes;
}

// MARK: - Reviews

static ReviewStatus review_from_states(const char *decision, bool changes, bool approved, bool commented, bool requested) {
    char *d = str_fold(decision);
    ReviewStatus s;
    if (str_eq(d, "approved")) s = REVIEW_APPROVED;
    else if (str_eq(d, "changes_requested")) s = REVIEW_CHANGES_REQUESTED;
    else if (changes) s = REVIEW_CHANGES_REQUESTED;
    else if (approved) s = REVIEW_APPROVED;
    else if (commented) s = REVIEW_FEEDBACK;
    else if (str_eq(d, "review_required") || requested) s = REVIEW_REQUESTED;
    else s = REVIEW_NONE;
    free(d);
    return s;
}
ReviewStatus review_status(const char *decision, const Json *reviews) {
    bool changes = false, approved = false, commented = false, requested = false;
    size_t n = json_count(reviews);
    for (size_t i = 0; i < n; i++) {
        char *state = str_fold(json_str(json_get(json_at(reviews, i), "state")));
        if (str_eq(state, "changes_requested")) changes = true;
        else if (str_eq(state, "approved")) approved = true;
        else if (str_eq(state, "commented")) commented = true;
        else if (str_eq(state, "requested")) requested = true;
        free(state);
    }
    return review_from_states(decision, changes, approved, commented, requested);
}
ReviewStatus review_status_of_reviewers(const char *decision, const Reviewer *reviewers, size_t count) {
    Json *list = json_array();
    for (size_t i = 0; i < count; i++) { Json *o = json_object(); json_set_str(o, "state", reviewers[i].state); json_array_push(list, o); }
    ReviewStatus s = review_status(decision, list);
    json_free(list);
    return s;
}
const char *review_status_text(ReviewStatus s) {
    switch (s) {
    case REVIEW_APPROVED: return "Approved";
    case REVIEW_CHANGES_REQUESTED: return "Changes requested";
    case REVIEW_FEEDBACK: return "Feedback given";
    case REVIEW_REQUESTED: return "Review requested";
    default: return "";
    }
}

// MARK: - Stacks

static void chain_parse(const Json *chain, StackPosition *out) {
    size_t n = json_count(chain);
    out->chain = xcalloc(n, sizeof *out->chain);
    for (size_t i = 0; i < n; i++) {
        const Json *item = json_at(chain, i);
        double number;
        if (!json_num(json_get(item, "number"), &number)) continue;
        StackItem *s = &out->chain[out->chain_count++];
        s->number = (int)number;
        const char *title = json_str(json_get(item, "title"));
        s->title = title ? xstrdup(title) : xstrfmt("Pull request #%d", s->number);
        const char *branch = json_str_nonempty(json_get(item, "branch"));
        if (!branch) branch = json_str_nonempty(json_get(item, "headRef"));
        s->branch = xstrdup(branch);
        s->depth = json_int_or(json_get(item, "depth"), 1);
        s->draft = json_bool_is(json_get(item, "draft"), true);
    }
}
static bool stack_header_parse(const Json *value, StackPosition *out) {
    memset(out, 0, sizeof *out);
    double position, total;
    if (!json_num(json_get(value, "position"), &position) || !json_num(json_get(value, "total"), &total)) return false;
    out->position = (int)position; out->total = (int)total; out->partial = json_bool_is(json_get(value, "partial"), true);
    return true;
}
bool stack_position_parse(const Json *value, const Json *stacks, StackPosition *out) {
    if (!stack_header_parse(value, out)) return false;
    char *id;
    double numeric;
    if (json_num(json_get(value, "id"), &numeric)) id = xstrfmt("%d", (int)numeric);
    else id = xstrdup(json_str_or(json_get(value, "id"), ""));
    chain_parse(json_get(stacks, id), out);
    free(id);
    return true;
}
bool stack_position_restore(const Json *value, StackPosition *out) {
    if (!stack_header_parse(value, out)) return false;
    const char *base = json_str_nonempty(json_get(value, "base"));
    out->base = xstrdup(base);
    chain_parse(json_get(value, "chain"), out);
    return true;
}
Json *stack_position_json(const StackPosition *s) {
    Json *value = json_object();
    json_set_num(value, "position", s->position); json_set_num(value, "total", s->total); json_set_bool(value, "partial", s->partial);
    if (s->base) json_set_str(value, "base", s->base);
    Json *chain = json_array();
    for (size_t i = 0; i < s->chain_count; i++) {
        const StackItem *item = &s->chain[i];
        Json *o = json_object();
        json_set_num(o, "number", item->number); json_set_str(o, "title", item->title); json_set_num(o, "depth", item->depth);
        if (item->branch) json_set_str(o, "branch", item->branch);
        if (item->draft) json_set_bool(o, "draft", true);
        json_array_push(chain, o);
    }
    json_object_set(value, "chain", chain);
    return value;
}
void stack_position_free(StackPosition *s) {
    if (!s) return;
    for (size_t i = 0; i < s->chain_count; i++) { free(s->chain[i].title); free(s->chain[i].branch); }
    free(s->chain); free(s->base); memset(s, 0, sizeof *s);
}
void stack_position_copy(StackPosition *into, const StackPosition *from) {
    memset(into, 0, sizeof *into);
    into->position = from->position; into->total = from->total; into->partial = from->partial;
    into->base = xstrdup(from->base);
    into->chain = xcalloc(from->chain_count, sizeof *into->chain);
    for (size_t i = 0; i < from->chain_count; i++) {
        into->chain[i] = from->chain[i];
        into->chain[i].title = xstrdup(from->chain[i].title);
        into->chain[i].branch = xstrdup(from->chain[i].branch);
    }
    into->chain_count = from->chain_count;
}
char *stack_position_label(const StackPosition *s, int number) {
    int depth = s->position;
    for (size_t i = 0; i < s->chain_count; i++) if (number && s->chain[i].number == number) { depth = s->chain[i].depth; break; }
    return xstrfmt("%d/%d%s", depth, s->total, s->partial ? "+" : "");
}
static const PullSummary *row_numbered(const PullSummary *rows, size_t count, int number) {
    for (size_t i = 0; i < count; i++) if (rows[i].number == number) return &rows[i];
    return NULL;
}
void stack_position_branches(StackPosition *s, const PullSummary *rows, size_t count) {
    const StackItem *bottom = NULL;
    for (size_t i = 0; i < s->chain_count; i++) {
        StackItem *item = &s->chain[i];
        const PullSummary *row = row_numbered(rows, count, item->number);
        if (row && !item->branch && !str_empty(row->branch)) item->branch = xstrdup(row->branch);
        if (!bottom || item->depth < bottom->depth) bottom = item;
    }
    // A chain only partly visible may not reach the bottom, whose base is then unknown.
    if (!bottom || (s->partial && bottom->depth > 1)) return;
    const PullSummary *row = row_numbered(rows, count, bottom->number);
    if (row && !str_empty(row->base_branch)) { free(s->base); s->base = xstrdup(row->base_branch); }
}
size_t *stack_position_top_first(const StackPosition *s) {
    size_t *order = xcalloc(s->chain_count, sizeof *order);
    for (size_t i = 0; i < s->chain_count; i++) order[i] = i;
    // Insertion sort by depth, deepest first; items of one depth keep the server's order.
    for (size_t i = 1; i < s->chain_count; i++) {
        size_t v = order[i], k = i;
        while (k > 0 && s->chain[order[k - 1]].depth < s->chain[v].depth) { order[k] = order[k - 1]; k--; }
        order[k] = v;
    }
    return order;
}

bool safe_web_url(const char *value) {
    if (!value || !str_has_prefix(value, "https://")) return false;
    const char *p = value + 8;
    const char *end = p; while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    if (end == p) return false;
    for (const char *q = p; q < end; q++) if (*q == '@') return false;
    return true;
}

// MARK: - ▶ Run

/// The array a list answer holds under `key`, or the answer itself when it is the array.
static const Json *list_of(const Json *answer, const char *key) { return json_is_array(answer) ? answer : json_get(answer, key); }

char **run_profiles_parse(const Json *projects, const char *repo, size_t *count) {
    const Json *rows = list_of(projects, "projects");
    char **out = NULL; size_t n = 0;
    for (size_t i = 0; i < json_count(rows) && !out; i++) {
        const Json *row = json_at(rows, i);
        if (!str_eq(json_str(json_get(row, "repo")), repo)) continue;
        const Json *names = json_get(row, "runProfiles");
        out = xcalloc(json_count(names) + 1, sizeof *out);
        for (size_t k = 0; k < json_count(names); k++) if (json_str_nonempty(json_at(names, k))) out[n++] = xstrdup(json_str(json_at(names, k)));
    }
    if (!out) out = xcalloc(1, sizeof *out);
    *count = n;
    return out;
}
char *run_session_preparing(const Json *sessions, int number) {
    const Json *rows = list_of(sessions, "sessions");
    const char *best = NULL, *best_at = NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        Session s; s.raw = (Json *)json_at(rows, i);
        const char *title = json_str(json_get(s.raw, "title")), *at = json_str_or(json_get(s.raw, "createdAt"), "");
        if (session_pull_number(&s) != number || !title || strncmp(title, "Run: #", 6) || !session_id(&s)) continue;
        if (!best || strcmp(at, best_at) > 0) { best = session_id(&s); best_at = at; }
    }
    return best ? xstrdup(best) : NULL;
}
bool run_session_serving(const Json *sessions, int number, char **session_id_out, char **url_out) {
    const Json *rows = list_of(sessions, "sessions");
    for (size_t i = 0; i < json_count(rows); i++) {
        Session s; s.raw = (Json *)json_at(rows, i);
        const char *url = json_str(json_get(json_at(json_get(s.raw, "serveLinks"), 0), "url"));
        if (session_pull_number(&s) != number || !safe_web_url(url) || !session_id(&s)) continue;
        *session_id_out = xstrdup(session_id(&s)); *url_out = xstrdup(url);
        return true;
    }
    return false;
}

/// A ▶ Run on a branch: a preview session with no pull request.
static bool branch_run(const Session *s) { return json_bool_is(json_get(s->raw, "preview"), true) && session_pull_number(s) == 0 && session_id(s); }
char *run_session_preparing_branch(const Json *sessions) {
    const Json *rows = list_of(sessions, "sessions");
    const char *best = NULL, *best_at = NULL;
    for (size_t i = 0; i < json_count(rows); i++) {
        Session s; s.raw = (Json *)json_at(rows, i);
        const char *at = json_str_or(json_get(s.raw, "createdAt"), "");
        if (!branch_run(&s)) continue;
        if (!best || strcmp(at, best_at) > 0) { best = session_id(&s); best_at = at; }
    }
    return best ? xstrdup(best) : NULL;
}
bool run_session_serving_branch(const Json *sessions, char **session_id_out, char **url_out) {
    const Json *rows = list_of(sessions, "sessions");
    for (size_t i = 0; i < json_count(rows); i++) {
        Session s; s.raw = (Json *)json_at(rows, i);
        const char *url = json_str(json_get(json_at(json_get(s.raw, "serveLinks"), 0), "url"));
        if (!branch_run(&s) || !safe_web_url(url)) continue;
        *session_id_out = xstrdup(session_id(&s)); *url_out = xstrdup(url);
        return true;
    }
    return false;
}

size_t run_log_add(RunLog *log, const char *text, bool error) {
    size_t added = 0;
    for (const char *p = text; p && *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t len = n && p[n - 1] == '\r' ? n - 1 : n;
        if (len) {
            if (log->count == RUN_LOG_CAP) {
                free(log->lines[0]);
                memmove(log->lines, log->lines + 1, (RUN_LOG_CAP - 1) * sizeof *log->lines);
                memmove(log->errors, log->errors + 1, (RUN_LOG_CAP - 1) * sizeof *log->errors);
                log->count--;
            }
            log->lines = xrealloc(log->lines, (log->count + 1) * sizeof *log->lines);
            log->errors = xrealloc(log->errors, (log->count + 1) * sizeof *log->errors);
            log->lines[log->count] = xstrndup(p, len); log->errors[log->count] = error; log->count++; added++;
        }
        p = e ? e + 1 : p + n;
    }
    return added;
}
bool run_log_add_events(RunLog *log, const Json *events) {
    size_t added = 0;
    for (size_t i = 0; i < json_count(events); i++) {
        const Json *e = json_at(events, i);
        double seq;
        if (json_num(json_get(e, "seq"), &seq)) { if (seq <= log->cursor) continue; log->cursor = seq; }
        const char *kind = json_str(json_get(e, "kind")), *text = json_str(json_get(e, "text"));
        if (str_eq(kind, "status")) {
            const char *status = json_str(json_get(e, "status"));
            if (status) { char *t = xstrfmt("\xE2\x80\xA2 %s", status); added += run_log_add(log, t, false); free(t); }
        } else if (str_eq(kind, "cmd") && text) {
            char *t = xstrfmt("$ %s", text); added += run_log_add(log, t, false); free(t);
        } else if (text && (str_eq(kind, "info") || str_eq(kind, "git") || str_eq(kind, "setup") || str_eq(kind, "claude") || str_eq(kind, "stderr"))) {
            added += run_log_add(log, text, str_eq(kind, "stderr"));
        }
    }
    return added > 0;
}
void run_log_clear(RunLog *log) {
    for (size_t i = 0; i < log->count; i++) free(log->lines[i]);
    free(log->lines); free(log->errors);
    memset(log, 0, sizeof *log);
}

// MARK: - Projects board

static bool project_field_parse(const Json *value, ProjectField *out) {
    memset(out, 0, sizeof *out);
    const char *name = json_str_nonempty(json_get(value, "name"));
    const Json *v = json_get(value, "value");
    double n;
    if (!name) return false;
    if (json_str(v)) out->value = xstrdup(json_str(v));
    else if (json_num(v, &n)) out->value = project_sum_text(n);
    else return false;
    out->name = xstrdup(name); out->color = json_dup_str(json_get(value, "color"));
    return true;
}
static void project_field_free(ProjectField *f) { free(f->name); free(f->value); free(f->color); memset(f, 0, sizeof *f); }
static DEFINE_LIST_PARSE(ProjectField, project_fields_parse, project_field_parse)
static DEFINE_LIST_FREE(ProjectField, project_fields_free, project_field_free)

static bool project_card_parse(const Json *value, ProjectCard *out) {
    memset(out, 0, sizeof *out);
    if (!json_is_object(value)) return false;
    out->type = xstrdup(json_str_or(json_get(value, "type"), "issue"));
    out->repo = json_dup_str(json_get(value, "repo"));
    out->number = json_int_or(json_get(value, "number"), 0);
    out->title = json_dup_str(json_get(value, "title"));
    out->url = json_dup_str(json_get(value, "url")); out->state = json_dup_str(json_get(value, "state"));
    out->author = json_dup_str(json_get(value, "author"));
    out->has_created = board_date_parse(json_str(json_get(value, "createdAt")), &out->created_at);
    const Json *people = json_get(value, "assignees");
    out->assignees = xcalloc(json_count(people) + 1, sizeof *out->assignees);
    for (size_t i = 0; i < json_count(people); i++) {
        const char *login = json_str_nonempty(json_get(json_at(people, i), "login"));
        if (!login) login = json_str_nonempty(json_at(people, i));
        if (login) out->assignees[out->assignee_count++] = xstrdup(login);
    }
    out->labels = labels_parse(json_get(value, "labels"), &out->label_count);
    out->has_parent = board_link_parse(json_get(value, "parent"), &out->parent);
    out->fields = project_fields_parse(json_get(value, "fields"), &out->field_count);
    return true;
}
static void project_card_free(ProjectCard *c) {
    free(c->type); free(c->repo); free(c->title); free(c->url); free(c->state); free(c->author);
    str_array_free(c->assignees, c->assignee_count);
    labels_free(c->labels, c->label_count);
    if (c->has_parent) board_link_free(&c->parent);
    project_fields_free(c->fields, c->field_count);
    memset(c, 0, sizeof *c);
}
static DEFINE_LIST_PARSE(ProjectCard, project_cards_parse, project_card_parse)
static DEFINE_LIST_FREE(ProjectCard, project_cards_free, project_card_free)

static bool project_column_parse(const Json *value, ProjectColumn *out) {
    memset(out, 0, sizeof *out);
    const char *name = json_str(json_get(value, "name"));
    if (!name) return false;
    out->name = xstrdup(name); out->color = json_dup_str(json_get(value, "color"));
    out->cards = project_cards_parse(json_get(value, "items"), &out->card_count);
    out->count = json_int_or(json_get(value, "count"), (int)out->card_count);
    const Json *sums = json_get(value, "sums");
    out->sums = xcalloc(json_count(sums) + 1, sizeof *out->sums);
    for (size_t i = 0; i < json_count(sums); i++) {
        double n;
        if (!json_key(sums, i) || !json_num(json_get(sums, json_key(sums, i)), &n)) continue;
        out->sums[out->sum_count].name = xstrdup(json_key(sums, i)); out->sums[out->sum_count++].value = n;
    }
    return true;
}
static void project_column_free(ProjectColumn *c) {
    free(c->name); free(c->color);
    for (size_t i = 0; i < c->sum_count; i++) free(c->sums[i].name);
    free(c->sums);
    project_cards_free(c->cards, c->card_count);
    memset(c, 0, sizeof *c);
}
static DEFINE_LIST_PARSE(ProjectColumn, project_columns_parse, project_column_parse)
static DEFINE_LIST_FREE(ProjectColumn, project_columns_free, project_column_free)

bool project_board_parse(const Json *value, ProjectBoard *out) {
    memset(out, 0, sizeof *out);
    if (!json_is_object(value)) return false;
    const Json *project = json_get(value, "project"), *view = json_get(value, "view");
    out->title = json_dup_str(json_get(project, "title")); out->url = json_dup_str(json_get(project, "url"));
    out->view_name = json_dup_str(json_get(view, "name")); out->view_url = json_dup_str(json_get(view, "url"));
    out->filter = json_dup_str(json_get(view, "filter"));
    out->group_by = json_dup_str(json_get(value, "groupBy"));
    out->columns = project_columns_parse(json_get(value, "columns"), &out->column_count);
    out->truncated = json_bool_is(json_get(value, "truncated"), true);
    out->error = json_str_nonempty(json_get(value, "projectsError")) ? xstrdup(json_str(json_get(value, "projectsError"))) : NULL;
    return true;
}
void project_board_free(ProjectBoard *b) {
    if (!b) return;
    free(b->title); free(b->url); free(b->view_name); free(b->view_url); free(b->filter); free(b->group_by); free(b->error);
    project_columns_free(b->columns, b->column_count);
    memset(b, 0, sizeof *b);
}
const ProjectField *project_card_field(const ProjectCard *card, const char *name) {
    for (size_t i = 0; i < card->field_count; i++) if (str_ieq(card->fields[i].name, name)) return &card->fields[i];
    return NULL;
}
bool project_card_assigned(const ProjectCard *card, const char *assignee) {
    if (str_empty(assignee)) return true;
    if (str_eq(assignee, PROJECT_NO_ASSIGNEE)) return card->assignee_count == 0;
    for (size_t i = 0; i < card->assignee_count; i++) if (str_ieq(card->assignees[i], assignee)) return true;
    return false;
}
BoardLink *project_card_pulls(const ProjectCard *card, const char *repo, const IssueSummary *issues, size_t issue_count,
                              const PullSummary *pulls, size_t pull_count, size_t *count) {
    *count = 0;
    if (!str_eq(card->type, "issue") || !card->repo || !fold_eq(card->repo, repo) || card->number < 1) return NULL;
    const IssueSummary *row = issues_find(issues, issue_count, card->number);
    BoardLink *out = xcalloc((row ? row->pull_count : 0) + pull_count + 1, sizeof *out);
    for (size_t i = 0; row && i < row->pull_count; i++) board_link_copy(&out[(*count)++], &row->pulls[i]);
    for (size_t i = 0; i < pull_count; i++) {
        const PullSummary *pull = &pulls[i];
        bool closes = false, listed = false;
        for (size_t k = 0; k < pull->issue_count && !closes; k++) closes = pull->issues[k].number == card->number && !board_link_is_foreign(&pull->issues[k], repo);
        for (size_t k = 0; k < *count && !listed; k++) listed = out[k].number == pull->number && !board_link_is_foreign(&out[k], repo);
        if (!closes || listed) continue;
        BoardLink *l = &out[(*count)++];
        l->number = pull->number; l->title = xstrdup(pull->title); l->url = xstrdup(pull->url);
        l->state = xstrdup("open"); l->draft = pull->draft;
    }
    return out;
}
FilterOption *project_board_assignees(const ProjectBoard *board, const char *pick, size_t *count) {
    FilterOption *options = NULL; size_t n = 0, cap = 0;
    int nobody = 0;
    for (size_t c = 0; c < board->column_count; c++) {
        const ProjectColumn *column = &board->columns[c];
        for (size_t k = 0; k < column->card_count; k++) {
            const ProjectCard *card = &column->cards[k];
            if (!card->assignee_count) nobody++;
            for (size_t a = 0; a < card->assignee_count; a++) {
                const char *login = card->assignees[a];
                bool dup = false;
                for (size_t b = 0; b < a && !dup; b++) dup = fold_eq(card->assignees[b], login);
                if (dup) continue;
                size_t i = 0;
                for (; i < n; i++) if (fold_eq(options[i].text, login)) break;
                if (i == n) {
                    if (n == cap) { cap = cap ? cap * 2 : 8; options = xrealloc(options, cap * sizeof *options); }
                    options[n].value = xstrdup(login); options[n].text = xstrdup(login); options[n].count = 0; n++;
                }
                options[i].count++;
            }
        }
    }
    if (!str_empty(pick) && !str_eq(pick, PROJECT_NO_ASSIGNEE)) {
        bool listed = false;
        for (size_t i = 0; i < n && !listed; i++) listed = fold_eq(options[i].value, pick);
        if (!listed) {
            if (n == cap) { cap = cap ? cap * 2 : 8; options = xrealloc(options, cap * sizeof *options); }
            options[n].value = xstrdup(pick); options[n].text = xstrdup(pick); options[n].count = 0; n++;
        }
    }
    if (n > 1) qsort(options, n, sizeof *options, compare_options);
    if (nobody || str_eq(pick, PROJECT_NO_ASSIGNEE)) {
        options = xrealloc(options, (n + 1) * sizeof *options);
        options[n].value = xstrdup(PROJECT_NO_ASSIGNEE); options[n].text = xstrdup("No assignee"); options[n].count = nobody; n++;
    }
    *count = n;
    return options;
}
int project_column_matching(const ProjectColumn *column, const char *assignee, double *sums) {
    int matching = 0;
    if (sums) for (size_t s = 0; s < column->sum_count; s++) sums[s] = 0;
    for (size_t k = 0; k < column->card_count; k++) {
        const ProjectCard *card = &column->cards[k];
        if (!project_card_assigned(card, assignee)) continue;
        matching++;
        for (size_t s = 0; sums && s < column->sum_count; s++) {
            const ProjectField *f = project_card_field(card, column->sums[s].name);
            if (f && f->value) sums[s] += strtod(f->value, NULL);
        }
    }
    return matching;
}
void project_board_keep_repo(ProjectBoard *board, const char *repo) {
    if (str_empty(repo)) return;
    for (size_t c = 0; c < board->column_count; c++) {
        ProjectColumn *column = &board->columns[c];
        size_t kept = 0;
        for (size_t k = 0; k < column->card_count; k++) {
            if (column->cards[k].repo && str_ieq(column->cards[k].repo, repo)) column->cards[kept++] = column->cards[k];
            else project_card_free(&column->cards[k]);
        }
        if (kept == column->card_count) continue;
        column->card_count = kept;
        double *sums = xcalloc(column->sum_count + 1, sizeof *sums);
        column->count = project_column_matching(column, NULL, sums);
        for (size_t s = 0; s < column->sum_count; s++) column->sums[s].value = sums[s];
        free(sums);
    }
}
bool project_color_rgb(const char *name, int rgb[3]) {
    // GitHub's Primer colours behind the option colours, as its board draws them.
    static const struct { const char *name; int rgb[3]; } colors[] = {
        { "GRAY", { 0x9A, 0xA0, 0xA6 } }, { "BLUE", { 0x40, 0x8C, 0xFF } }, { "GREEN", { 0x3F, 0xB9, 0x50 } },
        { "YELLOW", { 0xD2, 0x99, 0x22 } }, { "ORANGE", { 0xDB, 0x6D, 0x28 } }, { "RED", { 0xF8, 0x51, 0x49 } },
        { "PINK", { 0xDB, 0x61, 0xA2 } }, { "PURPLE", { 0xA3, 0x71, 0xF7 } },
    };
    for (size_t i = 0; name && i < sizeof colors / sizeof *colors; i++) {
        if (!str_ieq(colors[i].name, name)) continue;
        memcpy(rgb, colors[i].rgb, sizeof colors[i].rgb);
        return true;
    }
    return false;
}
char *project_sum_text(double value) {
    if (value == (double)(long long)value) return xstrfmt("%lld", (long long)value);
    char *text = xstrfmt("%.2f", value);
    size_t n = strlen(text);
    while (n && text[n - 1] == '0') text[--n] = 0;
    if (n && text[n - 1] == '.') text[--n] = 0;
    return text;
}

/// A run of digits that is the whole of [p, end), as a positive int; 0 otherwise.
static int whole_number(const char *p, const char *end) {
    if (p == end || end - p > 9) return 0;
    int n = 0;
    for (; p < end; p++) { if (!isdigit((unsigned char)*p)) return 0; n = n * 10 + (*p - '0'); }
    return n;
}
Json *project_board_setting_from_url(const char *url) {
    if (!url) return NULL;
    while (isspace((unsigned char)*url)) url++;
    const char *p = url;
    if (str_has_prefix(p, "https://")) p += 8; else if (str_has_prefix(p, "http://")) p += 7;
    if (!str_has_prefix(p, "github.com/")) return NULL;
    p += 11;
    const char *type;
    if (str_has_prefix(p, "orgs/")) { type = "organization"; p += 5; }
    else if (str_has_prefix(p, "users/")) { type = "user"; p += 6; }
    else return NULL;
    // orgs/<owner>/projects/<n>[/views/<v>], with anything after a ? or # left alone.
    size_t n;
    char *path = xstrndup(p, strcspn(p, "?# \t\r\n"));
    char **parts = str_split(path, '/', &n);
    free(path);
    Json *out = NULL;
    int number = n >= 3 && str_eq(parts[1], "projects") ? whole_number(parts[2], parts[2] + strlen(parts[2])) : 0;
    int view = n >= 5 && str_eq(parts[3], "views") ? whole_number(parts[4], parts[4] + strlen(parts[4])) : 0;
    if (n && *parts[0] && number && (n == 3 || (n == 4 && !*parts[3]) || (view && (n == 5 || (n == 6 && !*parts[5]))))) {
        out = json_object();
        json_set_str(out, "owner", parts[0]); json_set_str(out, "ownerType", type);
        json_set_num(out, "number", number);
        if (view) json_set_num(out, "view", view); else json_object_set(out, "view", json_null());
    }
    str_array_free(parts, n);
    return out;
}
char *project_board_setting_url(const Json *setting) {
    const char *owner = json_str_nonempty(json_get(setting, "owner"));
    int number = json_int_or(json_get(setting, "number"), 0), view = json_int_or(json_get(setting, "view"), 0);
    if (!owner || number < 1) return NULL;
    const char *kind = str_eq(json_str(json_get(setting, "ownerType")), "user") ? "users" : "orgs";
    return view > 0 ? xstrfmt("https://github.com/%s/%s/projects/%d/views/%d", kind, owner, number, view)
                    : xstrfmt("https://github.com/%s/%s/projects/%d", kind, owner, number);
}

// MARK: - Edits

static bool names_have(const Json *names, const char *name) {
    for (size_t i = 0; i < json_count(names); i++) if (fold_eq(json_str(json_at(names, i)), name)) return true;
    return false;
}
Json *board_names_parse(const char *text, bool logins) {
    Json *out = json_array();
    for (const char *p = text ? text : ""; *p;) {
        while (*p == ' ' || *p == '\t') p++;
        Str raw; str_init(&raw);
        if (*p == '"') {   // a quoted name keeps its commas; "" inside it is one quote
            for (p++; *p && !(*p == '"' && p[1] != '"'); p++) { str_appendc(&raw, *p); if (*p == '"') p++; }
            if (*p) p++;
        }
        size_t n = strcspn(p, ",\r\n");
        str_appendf(&raw, "%.*s", (int)n, p);
        char *joined = str_detach(&raw), *name = str_trim(joined);
        const char *kept = logins && *name == '@' ? name + 1 : name;
        if (*kept && !names_have(out, kept)) json_array_push(out, json_string(kept));
        free(joined); free(name);
        p += n;
        if (*p) p++;
    }
    return out;
}
/// A name as an edit box holds it: quoted when a comma, a line break or a leading quote would split or change it.
static void names_append(Str *out, const char *name) {
    if (!name[strcspn(name, ",\r\n")] && *name != '"') { str_appendz(out, name); return; }
    str_appendc(out, '"');
    for (const char *c = name; *c; c++) { if (*c == '"') str_appendc(out, '"'); str_appendc(out, *c); }
    str_appendc(out, '"');
}
char *board_names_join(char *const *names, size_t count) {
    Str out; str_init(&out);
    for (size_t i = 0; i < count; i++) { if (i) str_appendz(&out, ", "); names_append(&out, names[i]); }
    return str_detach(&out);
}
char *board_label_names_join(const PullLabel *labels, size_t count) {
    Str out; str_init(&out);
    for (size_t i = 0; i < count; i++) { if (i) str_appendz(&out, ", "); names_append(&out, labels[i].name ? labels[i].name : ""); }
    return str_detach(&out);
}
Json *board_assignees_toggle(char *const *assignees, size_t count, const char *login, bool *added) {
    Json *out = json_array();
    bool had = false;
    for (size_t i = 0; i < count; i++) {
        if (fold_eq(assignees[i], login)) { had = true; continue; }
        json_array_push(out, json_string(assignees[i]));
    }
    if (!had && login && *login) json_array_push(out, json_string(login));
    if (added) *added = !had;
    return out;
}
