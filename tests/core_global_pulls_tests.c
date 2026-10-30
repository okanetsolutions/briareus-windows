#include "global_pulls.h"
#include "str.h"
#include "test.h"
#include "suites.h"
#include <stdlib.h>
#include <string.h>

static void identity_order_filters(void) {
    GlobalPulls s = {0}; Project projects[] = {{"o/b", "Beta"}, {"O/A", "Alpha"}, {"o/a", "Duplicate"}};
    global_pulls_replace(&s, projects, 3); CHECK_INT(s.count, 2);
    Json *b = json_parsez("{\"repo\":\"o/b\",\"pulls\":[{\"number\":7,\"title\":\"B\",\"author\":\"bob\",\"checks\":\"failure\",\"updatedAt\":\"2026-10-30T12:00:00Z\",\"recommended\":\"fix-ci\"},{\"number\":7,\"title\":\"duplicate\"}]}");
    Json *a = json_parsez("{\"pulls\":[{\"number\":7,\"title\":\"A\",\"author\":\"alice\",\"labels\":[{\"name\":\"Ready\"}],\"updatedAt\":\"2026-10-30T12:00:00Z\"},{\"number\":9,\"title\":\"No date\"}]}");
    CHECK(global_pulls_adopt(&s.projects[0], b)); CHECK(global_pulls_adopt(&s.projects[1], a));
    CHECK_INT(s.projects[0].count, 1); CHECK_STR(s.projects[0].pulls[0].recommended, "fix-ci");
    size_t n; GlobalPullRow *rows = global_pulls_rows(&s, 100, NULL, false, &n);
    CHECK_INT(n, 3); CHECK_INT(rows[0].project, 1); CHECK_INT(rows[1].project, 0); CHECK_INT(rows[2].pull, 1); free(rows);
    BoardFilter filter; board_filter_init(&filter); board_filter_set(&filter, FILTER_LABEL, "ready");
    rows = global_pulls_rows(&s, 100, &filter, false, &n); CHECK_INT(n, 1); CHECK_INT(rows[0].project, 1); free(rows);
    board_filter_set(&filter, FILTER_LABEL, ""); filter.hide_failing_checks = true;
    rows = global_pulls_rows(&s, 100, &filter, false, &n); CHECK_INT(n, 2); free(rows);
    board_filter_set(&filter, FILTER_AUTHOR, "BOB"); rows = global_pulls_rows(&s, 100, &filter, false, &n); CHECK_INT(n, 0); free(rows);
    board_filter_free(&filter); json_free(a); json_free(b); global_pulls_free(&s);
}
static void replacement_and_bounds(void) {
    GlobalPulls s = {0}; Project projects[] = {{"o/a", "Old"}, {"o/b", "Removed"}};
    global_pulls_replace(&s, projects, 2);
    Json *board = json_parsez("{\"pulls\":[{\"number\":1,\"title\":\"Kept\"}]}");
    CHECK(global_pulls_adopt(&s.projects[0], board)); CHECK(global_pulls_adopt(&s.projects[1], board)); json_free(board);
    Project next[] = {{"O/A", "Renamed"}, {"o/c", "New"}}; global_pulls_replace(&s, next, 2);
    CHECK_STR(s.projects[0].project.label, "Renamed"); CHECK_INT(s.projects[0].count, 1); CHECK(!s.projects[1].loaded);
    board = json_parsez("{\"repo\":\"o/b\",\"pulls\":[]}"); CHECK(!global_pulls_adopt(&s.projects[0], board)); json_free(board);
    board = json_object(); CHECK(!global_pulls_adopt(&s.projects[0], board)); CHECK_INT(s.projects[0].count, 1);
    Json *pulls = json_array(); json_object_set(board, "pulls", pulls);
    for (int i = 1; i <= 503; i++) { Json *p = json_object(); json_set_num(p, "number", i); json_set_str(p, "title", "Bounded"); json_array_push(pulls, p); }
    CHECK(global_pulls_adopt(&s.projects[0], board)); CHECK_INT(s.projects[0].count, 500); CHECK(s.projects[0].truncated);
    global_pulls_forget(&s.projects[0]); CHECK(!s.projects[0].loaded); CHECK_INT(s.projects[0].count, 0);
    global_pulls_replace(&s, NULL, 0); CHECK_INT(s.count, 0); json_free(board); global_pulls_free(&s);
}
void core_global_pulls_tests(void) {
    test_run("global pulls scoped identity ordering and board filters", identity_order_filters);
    test_run("global pulls replace scope validate and bound snapshots", replacement_and_bounds);
}
