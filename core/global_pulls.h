#ifndef BRIAREUS_GLOBAL_PULLS_H
#define BRIAREUS_GLOBAL_PULLS_H
#include "board.h"
#include "models.h"

typedef enum { GLOBAL_TAB_PULLS, GLOBAL_TAB_ISSUES, GLOBAL_TAB_REVIEW, GLOBAL_TAB_COUNT } GlobalBoardTab;

enum { GLOBAL_PULLS_PROJECT_BATCH = 25, GLOBAL_PULLS_ROW_LIMIT = 500 };
// Owned project snapshots; row references remain valid until replace/adopt/free.
typedef struct {
    Project project;
    Json *board;
    PullSummary *pulls; size_t count;
    IssueSummary *issues; size_t issue_count;
    char *issues_error; bool issues_truncated;
    char *error;
    time_t synced_at;
    bool loaded, truncated, fresh;
} GlobalPullProject;
typedef struct { GlobalPullProject *projects; size_t count; } GlobalPulls;
typedef struct { size_t project, pull; } GlobalPullRow;
void global_pulls_free(GlobalPulls *state);
// Removes absent repositories immediately, retaining matching snapshots case insensitively.
void global_pulls_replace(GlobalPulls *state, const Project *projects, size_t count);
bool global_pulls_adopt(GlobalPullProject *project, const Json *board);
void global_pulls_forget(GlobalPullProject *project);
// New allocated rows sorted by updated time descending, folded repo ascending, number descending.
GlobalPullRow *global_pulls_rows(const GlobalPulls *state, size_t limit, const BoardFilter *filter, bool hide_stacked, size_t *count);
// Same aggregate, active list semantics; Review List ignores ordinary filters.
GlobalPullRow *global_board_rows(const GlobalPulls *state, size_t limit, GlobalBoardTab tab, const BoardFilter *filter, bool hide_stacked, const char *login, size_t *count);
BoardRow global_board_row(const GlobalPulls *state, GlobalPullRow row, GlobalBoardTab tab);
bool global_pull_stack_passes(const PullSummary *pull, bool hide_stacked);
#endif
