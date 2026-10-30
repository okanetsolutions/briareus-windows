#ifndef BRIAREUS_CI_H
#define BRIAREUS_CI_H
#include "json.h"
#include "stats.h"
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
