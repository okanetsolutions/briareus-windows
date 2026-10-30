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
static void issue_and_review_semantics(void) {
    GlobalPulls s = {0}; Project projects[] = {{"o/a", "Alpha"}, {"o/b", "Beta"}};
    global_pulls_replace(&s, projects, 2);
    Json *board = json_parsez("{\"author\":\"alice\",\"pulls\":[{\"number\":7,\"title\":\"Review\",\"labels\":[{\"name\":\"requires-dev-review\"}]}],\"issues\":[{\"number\":7,\"title\":\"Issue A\",\"author\":\"sam\",\"assignees\":[\"alice\"],\"labels\":[{\"name\":\"ready\"}],\"updatedAt\":\"2026-10-30T12:00:00Z\"},{\"number\":7,\"title\":\"Duplicate\"},{\"number\":8,\"title\":\"Epic\",\"subIssues\":{\"total\":3,\"completed\":1}}]}" );
    CHECK(global_pulls_adopt(&s.projects[0], board)); CHECK_INT(s.projects[0].issue_count, 2); CHECK(issue_is_epic(&s.projects[0].issues[1])); CHECK_INT(s.projects[0].issues[1].sub_issues_done, 1);
    CHECK(global_pulls_adopt(&s.projects[1], board)); json_free(board);
    size_t n; BoardFilter filter; board_filter_init(&filter);
    board_filter_set(&filter, FILTER_ASSIGNEE, "ALICE");
    GlobalPullRow *rows = global_board_rows(&s, 2, GLOBAL_TAB_ISSUES, &filter, true, NULL, &n);
    CHECK_INT(n, 2); CHECK_INT(rows[0].project, 0); CHECK_INT(rows[1].project, 1);
    CHECK_STR(s.projects[rows[0].project].issues[rows[0].pull].title, "Issue A"); free(rows);
    board_filter_set(&filter, FILTER_AUTHOR, "nobody");
    rows = global_board_rows(&s, 2, GLOBAL_TAB_ISSUES, &filter, false, NULL, &n); CHECK_INT(n, 0); free(rows);
    rows = global_board_rows(&s, 2, GLOBAL_TAB_REVIEW, &filter, true, NULL, &n); CHECK_INT(n, 2); free(rows); // Project-author fallback, unfiltered.
    Json *refused = json_parsez("{\"pulls\":[{\"number\":7,\"title\":\"Still usable\"}],\"issues\":[{\"number\":7,\"title\":\"Must disappear\"}],\"issuesError\":\"Issues: read required\"}");
    CHECK(global_pulls_adopt(&s.projects[1], refused)); CHECK_INT(s.projects[1].issue_count, 0); CHECK_INT(s.projects[1].count, 1);
    CHECK_STR(s.projects[1].issues_error, "Issues: read required"); json_free(refused);
    board = json_parsez("{\"pulls\":[],\"issues\":[],\"issuesTruncated\":true}");
    CHECK(global_pulls_adopt(&s.projects[1], board)); CHECK(s.projects[1].issues_truncated); CHECK(s.projects[1].issues_error == NULL); json_free(board);
    board_filter_free(&filter); global_pulls_free(&s);
}
static void review_identity_and_bounds(void) {
    GlobalPulls s = {0}; Project project = {"o/a", "Alpha"}; global_pulls_replace(&s, &project, 1);
    Json *board = json_parsez("{\"pulls\":[{\"number\":1,\"title\":\"First\",\"assignees\":[\"alice\"],\"labels\":[{\"name\":\"requires-dev-review\"}]},{\"number\":2,\"title\":\"Answer\",\"reviewers\":[{\"user\":\"alice\",\"state\":\"CHANGES_REQUESTED\"}],\"labels\":[{\"name\":\"feedback-implemented\"}]}],\"issues\":[]}");
    CHECK(global_pulls_adopt(&s.projects[0], board)); size_t n;
    GlobalPullRow *rows = global_board_rows(&s, 1, GLOBAL_TAB_REVIEW, NULL, false, NULL, &n); CHECK_INT(n, 0); free(rows);
    rows = global_board_rows(&s, 1, GLOBAL_TAB_REVIEW, NULL, false, "ALICE", &n); CHECK_INT(n, 1); CHECK_INT(s.projects[0].pulls[rows[0].pull].number, 2); free(rows);
    Json *issues = json_array(); json_object_set(board, "issues", issues);
    for (int i = 1; i <= 502; i++) { Json *issue = json_object(); json_set_num(issue, "number", i); json_set_str(issue, "title", "Bounded issue"); json_array_push(issues, issue); }
    CHECK(global_pulls_adopt(&s.projects[0], board)); CHECK_INT(s.projects[0].issue_count, 500); CHECK(s.projects[0].issues_truncated);
    json_free(board); global_pulls_forget(&s.projects[0]); CHECK_INT(s.projects[0].issue_count, 0); CHECK(!s.projects[0].issues_truncated);
    board = json_parsez("{\"pulls\":[]}"); CHECK(global_pulls_adopt(&s.projects[0], board)); CHECK(s.projects[0].issues_error != NULL);
    json_free(board); global_pulls_free(&s);
}
void core_global_pulls_tests(void) {
    test_run("global issues and Review List semantics", issue_and_review_semantics);
    test_run("global review login identity and issue bounds", review_identity_and_bounds);
    test_run("global pulls scoped identity ordering and board filters", identity_order_filters);
    test_run("global pulls replace scope validate and bound snapshots", replacement_and_bounds);
}
