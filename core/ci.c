#include "ci.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

Json *ci_query(int days, double until_ms, bool statuses, const char *repo, const char *branch) {
    Json *q = json_object();
    json_set_num(q, "since", until_ms - days * 86400000.0); json_set_num(q, "until", until_ms);
    json_set_str(q, "kind", statuses ? "status" : "check_run");
    if (repo && *repo) json_set_str(q, "repo", repo);
    if (branch && *branch) json_set_str(q, "branch", branch);
    return q;
}
Json *ci_runs_query(const Json *window, int offset) {
    Json *q = json_clone(window); json_set_num(q, "limit", 50); json_set_num(q, "offset", offset); return q;
}
static bool groups(const Json *rows, const char *key) {
    if (!json_is_array(rows)) return false;
    for (size_t i = 0; i < json_count(rows); i++)
        if (!json_str_nonempty(json_get(json_at(rows, i), key))) return false;
    return true;
}
Json *ci_stats_parse(const Json *answer) {
    if (!json_is_object(json_get(answer, "totals"))) return NULL;
    if (!groups(json_get(answer, "daily"), "day") || !groups(json_get(answer, "repositories"), "repo")) return NULL;
    if (!groups(json_get(answer, "checks"), "name")) return NULL;
    return json_clone(answer);
}
Json *ci_page_parse(const Json *answer, int offset) {
    const Json *rows = json_get(answer, "runs"), *next = json_get(answer, "nextOffset");
    if (!json_is_array(rows) || json_count(rows) > 100) return NULL;
    if (!json_is_null(next)) {
        int n = json_int_or(next, -1);
        if (n <= offset || n > 100000 || !json_count(rows)) return NULL;
    }
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *r = json_at(rows, i), *id = json_get(r, "id"); double number;
        bool valid_id = json_str_nonempty(id) != NULL;
        if (json_num(id, &number)) valid_id = isfinite(number) && number >= 0 && floor(number) == number;
        if (!valid_id || !json_str_nonempty(json_get(r, "name"))) return NULL;
        if (!json_str_nonempty(json_get(r, "repo")) || !json_str_nonempty(json_get(r, "status"))) return NULL;
    }
    return json_clone(answer);
}
const char *ci_run_state(const Json *run) {
    if (!str_eq(json_str(json_get(run, "status")), "completed")) return "Running";
    const char *c = json_str(json_get(run, "conclusion"));
    if (str_eq(c, "success")) return "Passed";
    if (str_eq(c, "failure") || str_eq(c, "error") || str_eq(c, "timed_out")) return "Failed";
    if (str_eq(c, "startup_failure") || str_eq(c, "action_required")) return "Failed";
    if (str_eq(c, "cancelled")) return "Cancelled";
    if (str_eq(c, "skipped") || str_eq(c, "neutral")) return "Skipped";
    return "Other";
}
char *ci_metric_text(const Json *metrics, const char *key) {
    double n;
    if (!json_num(json_get(metrics, key), &n) || !isfinite(n) || n < 0) return xstrdup("Unknown");
    if (str_eq(key, "successRate")) return n <= 1 ? xstrfmt("%.0f%%", n * 100) : xstrdup("Unknown");
    if (strstr(key, "DurationMs") || str_eq(key, "durationMs")) return xstrfmt("%.1f s", n / 1000);
    return xstrfmt("%.0f", n);
}
char *ci_run_meta(const Json *run) {
    char *duration = ci_metric_text(run, "durationMs");
    double ms = json_num_or(json_get(run, "runAt"), 0);
    char date[40] = "date unknown";
    if (isfinite(ms) && ms > 0 && ms <= 253402300799000.0) {
        time_t seconds = (time_t)(ms / 1000); struct tm *utc = gmtime(&seconds);
        if (utc) strftime(date, sizeof date, "%Y-%m-%d %H:%M UTC", utc);
    }
    char *text = xstrfmt("%s · %s · %.7s · %s · %s", json_str_or(json_get(run, "repo"), ""),
        json_str_or(json_get(run, "branch"), "branch unknown"), json_str_or(json_get(run, "headSha"), ""), duration, date);
    free(duration); return text;
}
