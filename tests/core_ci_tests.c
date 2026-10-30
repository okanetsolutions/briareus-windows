#include "ci.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
static void query_contract(void) {
    Json *q = ci_query(7, 1793188800000.0, true, "acme/shop", "feature/a b");
    CHECK_STR(json_str(json_get(q, "kind")), "status"); CHECK_STR(json_str(json_get(q, "repo")), "acme/shop");
    CHECK_STR(json_str(json_get(q, "branch")), "feature/a b"); CHECK(json_num_or(json_get(q, "since"), 0) == 1792584000000.0);
    Json *page = ci_runs_query(q, 50); CHECK_INT(json_int_or(json_get(page, "offset"), 0), 50);
    CHECK_INT(json_int_or(json_get(page, "limit"), 0), 50); CHECK(json_equal(json_get(q, "until"), json_get(page, "until")));
    CHECK(json_is_null(json_get(q, "offset"))); json_free(page); json_free(q);
    q = ci_query(30, 1793188800000.0, false, "", NULL); CHECK(json_is_null(json_get(q, "repo"))); CHECK(json_is_null(json_get(q, "branch")));
    CHECK_STR(json_str(json_get(q, "kind")), "check_run"); json_free(q);
}
static void parsing(void) {
    const char *bad[] = {"{}", "{\"totals\":{},\"daily\":[],\"repositories\":[]}",
        "{\"totals\":{},\"daily\":[{}],\"repositories\":[],\"checks\":[]}",
        "{\"totals\":{},\"daily\":[],\"repositories\":{},\"checks\":[]}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { Json *j = json_parsez(bad[i]); CHECK(ci_stats_parse(j) == NULL); json_free(j); }
    Json *j = json_parsez("{\"totals\":{\"total\":0,\"successRate\":null},\"daily\":[],\"repositories\":[],\"checks\":[]}");
    Json *copy = ci_stats_parse(j); CHECK(copy != NULL); json_free(j);
    CHECK_OWNED_STR(ci_metric_text(json_get(copy, "totals"), "total"), "0");
    CHECK_OWNED_STR(ci_metric_text(json_get(copy, "totals"), "successRate"), "Unknown"); json_free(copy);
    const char *pages[] = {"{}", "{\"runs\":[],\"nextOffset\":50}", "{\"runs\":[{}]}",
        "{\"runs\":[{\"id\":1,\"name\":\"Build\",\"repo\":\"a/b\",\"status\":\"completed\"}],\"nextOffset\":50}",
        "{\"runs\":[{\"id\":null,\"name\":\"Build\"}]}",
        "{\"runs\":[{\"id\":\"2\",\"name\":\"Build\"}]}"};
    for (size_t i = 0; i < sizeof pages / sizeof *pages; i++) { j = json_parsez(pages[i]); CHECK(ci_page_parse(j, 50) == NULL); json_free(j); }
    j = json_parsez(pages[3]); copy = ci_page_parse(j, 0); CHECK(copy != NULL); json_free(copy); json_free(j);
    j = json_parsez("{\"runs\":[],\"nextOffset\":null,\"truncated\":true}"); copy = ci_page_parse(j, 0); CHECK(copy != NULL); json_free(copy); json_free(j);
    j = json_parsez("{\"runs\":[{\"id\":\"a\",\"name\":\"Build\",\"repo\":\"a/b\",\"status\":\"queued\"}],\"nextOffset\":100001}"); CHECK(ci_page_parse(j, 0) == NULL);
    json_set_num(j, "nextOffset", 1.5); CHECK(ci_page_parse(j, 0) == NULL); json_object_remove(j, "nextOffset");
    copy = ci_page_parse(j, 0); CHECK(copy != NULL); json_free(copy); json_free(j);
}
static void labels_and_unknowns(void) {
    Json *j = json_object(); CHECK_STR(ci_run_state(j), "Running"); json_set_str(j, "status", "completed");
    const char *values[] = {"success", "failure", "error", "timed_out", "startup_failure", "action_required", "cancelled", "skipped", "neutral", "new"};
    const char *expected[] = {"Passed", "Failed", "Failed", "Failed", "Failed", "Failed", "Cancelled", "Skipped", "Skipped", "Other"};
    for (size_t i = 0; i < 10; i++) { json_set_str(j, "conclusion", values[i]); CHECK_STR(ci_run_state(j), expected[i]); }
    json_set_num(j, "successRate", .92); CHECK_OWNED_STR(ci_metric_text(j, "successRate"), "92%");
    json_set_num(j, "successRate", 2); CHECK_OWNED_STR(ci_metric_text(j, "successRate"), "Unknown");
    json_set_num(j, "avgDurationMs", 1234); CHECK_OWNED_STR(ci_metric_text(j, "avgDurationMs"), "1.2 s");
    json_set_num(j, "total", -1); CHECK_OWNED_STR(ci_metric_text(j, "total"), "Unknown");
    char *meta = ci_run_meta(j); CHECK(strstr(meta, "branch unknown") != NULL); free(meta);
    json_set_str(j, "repo", "acme/shop"); json_set_str(j, "branch", "main"); json_set_str(j, "headSha", "abcdef12345");
    json_set_num(j, "durationMs", 0); json_set_num(j, "runAt", 1793188800000.0);
    meta = ci_run_meta(j); CHECK(strstr(meta, "acme/shop · main · abcdef1 · 0.0 s") != NULL); CHECK(strstr(meta, "2026-10-28 12:00 UTC") != NULL); free(meta); json_free(j);
}
void core_ci_tests(void) {
    test_run("CI query window", query_contract); test_run("CI response validation", parsing); test_run("CI states and unknown metrics", labels_and_unknowns);
}
