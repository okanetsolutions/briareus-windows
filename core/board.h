// The project board: what `pulls` answers, read into the rows the dashboard draws.
#ifndef BRIAREUS_BOARD_H
#define BRIAREUS_BOARD_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef struct { char *name; char *color; } PullLabel;
bool pull_label_parse(const Json *value, PullLabel *out);
void pull_label_free(PullLabel *label);
/// GitHub's own colour as red, green and blue in 0...255; false when it sent none that can be read.
bool pull_label_rgb(const PullLabel *label, int rgb[3]);

/// An issue or pull request another row points at.
typedef struct {
    int number;
    char *title, *url, *repo, *state, *state_reason;
    bool draft;
    PullLabel *labels; size_t label_count;
} BoardLink;
bool board_link_parse(const Json *value, BoardLink *out);
void board_link_free(BoardLink *link);
void board_link_copy(BoardLink *into, const BoardLink *from);
bool board_link_is_foreign(const BoardLink *link, const char *repo);
/// "#4" would read as this repository's #4, so one from elsewhere names its own. New string.
char *board_link_reference(const BoardLink *link, const char *repo);
bool board_link_not_planned(const BoardLink *link);

typedef struct { char *user; char *state; } Reviewer;

typedef struct {
    int number;
    char *title, *url, *branch, *base_branch, *author;
    bool draft;
    char **assignees; size_t assignee_count;
    Reviewer *reviewers; size_t reviewer_count;
    PullLabel *labels; size_t label_count;
    BoardLink *issues; size_t issue_count;
    char *mergeable;         // mergeable, conflicting, or unknown while GitHub is still computing the merge
    char *checks;            // success, failure, error, pending or expected; NULL without checks
    char *review_decision;
    char *recommended;       // the errand this pull request's state and labels ask for
    bool has_updated; time_t updated_at;
    Json *raw;               // the row as the server sent it, which is what is saved and what names its stack
} PullSummary;
bool pull_summary_parse(const Json *value, PullSummary *out);
void pull_summary_free(PullSummary *pull);
void pull_summary_copy(PullSummary *into, const PullSummary *from);
bool pull_conflicting(const PullSummary *pull);
/// The label is set by whoever saw the conflict first and may be ahead of GitHub's own answer.
bool pull_has_conflicts(const PullSummary *pull);
/// Red only: a run still going has nothing to fix yet.
bool pull_checks_failed(const PullSummary *pull);
bool pull_awaits_feedback(const PullSummary *pull);
/// Every valid row of a `pulls` array.
PullSummary *pull_summaries_parse(const Json *array, size_t *count);
void pull_summaries_free(PullSummary *pulls, size_t count);

typedef struct {
    int number;
    char *title, *url, *author, *milestone;
    char **assignees; size_t assignee_count;
    PullLabel *labels; size_t label_count;
    int comments;
    bool has_created; time_t created_at;
    bool has_updated; time_t updated_at;
    bool has_parent; BoardLink parent;
    int sub_issues, sub_issues_done;   // sub-issues GitHub tracks under an epic, closed ones included; zero on an ordinary issue
    BoardLink *pulls; size_t pull_count;   // the open pull requests that say they close this issue
} IssueSummary;
bool issue_summary_parse(const Json *value, IssueSummary *out);
void issue_summary_free(IssueSummary *issue);
bool issue_is_epic(const IssueSummary *issue);
IssueSummary *issue_summaries_parse(const Json *array, size_t *count);
void issue_summaries_free(IssueSummary *issues, size_t count);
typedef struct { size_t index; int depth; } IssueRow;
/// Sub-issues drawn under their epic, depth first, each with how deep it sits.
/// Only a parent on the list itself nests its children; a cycle leaves its rows flat.
IssueRow *issues_nested(const IssueSummary *issues, size_t count, const char *repo, size_t *row_count);
/// What a session started on this issue is sent, as the dashboard words it. Its first line names the session.
char *issue_prompt(const IssueSummary *issue, const char *repo);
/// The board row of issue `number`, or NULL when it is not on the list (closed, or past the issue walk's last page).
const IssueSummary *issues_find(const IssueSummary *issues, size_t count, int number);
/// The rows on the list that are sub-issues of `epic` in `repo`, as indices in list order. Closed ones are never there.
size_t *issue_open_sub_issues(const IssueSummary *issues, size_t count, int epic, const char *repo, size_t *found);
/// The board row of this repository's pull request `number`, or NULL.
const PullSummary *pulls_find(const PullSummary *pulls, size_t count, int number);

/// What the board's pickers filter on, carried by pull requests and issues alike.
typedef struct {
    const char *author;
    const Reviewer *reviewers; size_t reviewer_count;
    const PullLabel *labels; size_t label_count;
} BoardRow;
BoardRow pull_board_row(const PullSummary *pull);
BoardRow issue_board_row(const IssueSummary *issue);

typedef enum { FILTER_AUTHOR, FILTER_REVIEWER, FILTER_LABEL, FILTER_KIND_COUNT } FilterKind;
/// One author, reviewer and label the board is narrowed to; empty means all. Values are kept folded.
typedef struct { char *author, *reviewer, *label; } BoardFilter;
typedef struct { char *value, *text; int count; } FilterOption;
void board_filter_init(BoardFilter *filter);
void board_filter_free(BoardFilter *filter);
void board_filter_copy(BoardFilter *into, const BoardFilter *from);
bool board_filter_equal(const BoardFilter *a, const BoardFilter *b);
/// The board opens on the project's configured author, but only while they have something open.
void board_filter_opening(BoardFilter *filter, const char *author, const BoardRow *rows, size_t count);
bool board_filter_is_on(const BoardFilter *filter);
const char *board_filter_get(const BoardFilter *filter, FilterKind kind);
void board_filter_set(BoardFilter *filter, FilterKind kind, const char *value);
/// `skipping` (or -1) leaves one picker out, which is how each counts what it would show without counting itself.
bool board_filter_passes(const BoardFilter *filter, const BoardRow *row, int skipping);
/// What one picker offers, each counted against the other two. A pick they have emptied still lists itself.
FilterOption *board_filter_options(const BoardFilter *filter, FilterKind kind, const BoardRow *rows, size_t count, size_t *option_count);
void filter_options_free(FilterOption *options, size_t count);
const char *filter_kind_name(FilterKind kind);

/// An errand the board runs on a pull request: a paid session started with a prompt the server owns.
typedef struct { char *label, *placeholder; bool required; } ActionInput;
typedef struct { char *id, *label, *hint; bool has_input; ActionInput input; } BoardAction;
void board_action_free(BoardAction *action);
void board_action_copy(BoardAction *into, const BoardAction *from);
/// The call that starts it: `serve_pull` for Run, `review` for Code review, and `action` (with the errand's id) for the rest.
char *board_action_operation(const BoardAction *action);
/// Run answers only once the workspace is prepared and serving, which takes longer than a request is given. 0 = default.
int board_action_timeout_ms(const BoardAction *action);
/// Review checks the branch out itself; the rest look the pull request up by number.
Json *board_action_arguments(const BoardAction *action, const char *repo, int number, const char *branch, const char *input);
/// The board's errands, in the order the dashboard shows them.
const BoardAction *board_actions_known(size_t *count);
/// The errands worth offering on one pull request. `catalog` is what the server's `actions` lists; NULL or empty before it
/// is known, when every errand this app knows is offered.
BoardAction *board_actions_offered(const Json *catalog, const PullSummary *pull, int failed_checks, size_t *count);
void board_actions_free(BoardAction *actions, size_t count);

/// What is worth a word before a merge is confirmed. Returns a NULL-terminated array of new strings.
char **merge_warnings(const Json *mergeable, const char *state, size_t *count);

typedef enum { REVIEW_NONE, REVIEW_APPROVED, REVIEW_CHANGES_REQUESTED, REVIEW_FEEDBACK, REVIEW_REQUESTED } ReviewStatus;
/// The overall verdict a pull request carries, read from GitHub's review decision and the reviewers themselves.
ReviewStatus review_status(const char *decision, const Json *reviews);
ReviewStatus review_status_of_reviewers(const char *decision, const Reviewer *reviewers, size_t count);
const char *review_status_text(ReviewStatus status);

/// Where a pull request sits in a stack of branches built on each other, 1 being the bottom. `branch` and `base` (the
/// branch the bottom merges into) are NULL until the board's rows fill them in.
typedef struct { int number; char *title, *branch; int depth; bool draft; } StackItem;
typedef struct { int position, total; bool partial; char *base; StackItem *chain; size_t chain_count; } StackPosition;
bool stack_position_parse(const Json *value, const Json *stacks, StackPosition *out);
void stack_position_free(StackPosition *stack);
void stack_position_copy(StackPosition *into, const StackPosition *from);
/// "2/3" or "2/3+"; `number` 0 asks for the stack's own position.
char *stack_position_label(const StackPosition *stack, int number);
/// Each item's branch, and the branch the bottom one merges into, from the rows the board lists them on.
void stack_position_branches(StackPosition *stack, const PullSummary *rows, size_t count);
/// The stack as one object, chain and branches included, so a saved pull request opens with its overview.
Json *stack_position_json(const StackPosition *stack);
bool stack_position_restore(const Json *value, StackPosition *out);
/// The chain top first, as GitHub's stack popover lists it: the index of the item at each row.
size_t *stack_position_top_first(const StackPosition *stack);

/// An https URL with a host and no credentials, or NULL.
bool safe_web_url(const char *value);

// MARK: - ▶ Run

/// A project's run profiles, the default first, from what `projects` answers (the object or its array). A
/// NULL-terminated array of new strings; empty when the project lists none or is not listed.
char **run_profiles_parse(const Json *projects, const char *repo, size_t *count);
/// The session a ▶ Run on pull request `number` is preparing while `serve_pull` waits: the newest of its sessions titled
/// "Run: #…", from what `sessions` answers. A new string, or NULL.
char *run_session_preparing(const Json *sessions, int number);
/// A ▶ Run already serving pull request `number`: one of its sessions with a serve link. Sets new strings.
bool run_session_serving(const Json *sessions, int number, char **session_id, char **url);

/// The same two for a ▶ Run on a branch (the board's Run tab, on the default branch): its sessions are previews with no
/// pull request.
char *run_session_preparing_branch(const Json *sessions);
bool run_session_serving_branch(const Json *sessions, char **session_id, char **url);

/// The Run tab's log: the log lines of a session's transcript, one entry per line of text, the latest RUN_LOG_CAP.
enum { RUN_LOG_CAP = 400 };
typedef struct { char **lines; bool *errors; size_t count; double cursor; } RunLog;
/// Adds `text`, one entry per line; how many lines it added.
size_t run_log_add(RunLog *log, const char *text, bool error);
/// Adds the log lines among transcript events past the cursor (`info`, `cmd` as `$ …`, `git`, `setup`, `claude`,
/// `stderr` as errors, `status` as `• …`; the conversation's own kinds are skipped) and moves the cursor on. True
/// when a line was added.
bool run_log_add_events(RunLog *log, const Json *events);
/// Empties the log and rewinds its cursor.
void run_log_clear(RunLog *log);

#endif
