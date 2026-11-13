#ifndef BRIAREUS_CI_H
#define BRIAREUS_CI_H
#include "json.h"
#include "stats.h"
enum { CI_DEFAULT_DAYS = 30 };
// Clear filters means every project, Checks and every branch, keeping a window longer than the default: a shorter one
// holds no run the longer one did not, so clearing never narrows it. Empty or NULL repo/branch mean all.
int ci_cleared_days(int days);
// Whether clearing would change anything, so an empty answer offers Clear filters.
bool ci_filtered(int days, bool statuses, const char *repo, const char *branch);
// Whether validated figures observed no checks at all; an unknown total is not empty.
bool ci_stats_empty(const Json *stats);
// After a failed read of runs (`failed`): the page Retry reads again, the first while none are listed, else the next
// page, which a failed Load more leaves in place. -1 when the last read did not fail or no page is left.
int ci_retry_offset(bool failed, bool listed, int next_offset);
// Load more is offered while there is a next page and no failed read, which Retry reads instead.
bool ci_can_load_more(bool failed, int next_offset);
// Owned queries. Pin until_ms for both aggregates and every page; repo/branch are optional.
Json *ci_query(int days, double until_ms, bool statuses, const char *repo, const char *branch);
Json *ci_runs_query(const Json *window, int offset);
// Owned validated clones; NULL means malformed, not an empty success.
Json *ci_stats_parse(const Json *answer);
Json *ci_page_parse(const Json *answer, int offset);
// Borrowed state label; owned display strings. Missing/invalid numbers stay unknown.
const char *ci_run_state(const Json *run);
char *ci_metric_text(const Json *metrics, const char *key);
char *ci_run_meta(const Json *run);
#endif
