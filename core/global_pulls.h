#ifndef BRIAREUS_GLOBAL_PULLS_H
#define BRIAREUS_GLOBAL_PULLS_H
#include "board.h"
#include "models.h"

enum { GLOBAL_PULLS_PROJECT_BATCH = 25, GLOBAL_PULLS_ROW_LIMIT = 500 };
// Owned project snapshots; row references remain valid until replace/adopt/free.
typedef struct {
    Project project;
    Json *board;
    PullSummary *pulls; size_t count;
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
bool global_pull_stack_passes(const PullSummary *pull, bool hide_stacked);
#endif
