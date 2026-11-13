// The Issues tab's pages past the board (core/issue_pages.c): when Load more is offered, where each page starts, and
// how the pages join the board's rows.
#include "issue_pages.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void rows_into(Str *s, const int *numbers, size_t count) {
    str_appendz(s, "[");
    for (size_t i = 0; i < count; i++) str_appendf(s, "%s{\"number\":%d,\"title\":\"Issue %d\"}", i ? "," : "", numbers[i], numbers[i]);
    str_appendz(s, "]");
}
static Json *board(const int *numbers, size_t count, bool truncated, const char *error) {
    Str s; str_init(&s);
    str_appendz(&s, "{\"issues\":"); rows_into(&s, numbers, count);
    str_appendf(&s, ",\"issuesTruncated\":%s", truncated ? "true" : "false");
    if (error) str_appendf(&s, ",\"issuesError\":\"%s\"", error);
    str_appendz(&s, "}");
    Json *j = json_parsez(s.data); str_free(&s); return j;
}
static Json *page(const int *numbers, size_t count, int next, bool truncated) {
    Str s; str_init(&s);
    str_appendz(&s, "{\"repo\":\"o/r\",\"issues\":"); rows_into(&s, numbers, count);
    if (next >= 0) str_appendf(&s, ",\"nextOffset\":%d", next); else str_appendz(&s, ",\"nextOffset\":null");
    if (truncated) str_appendz(&s, ",\"issuesTruncated\":true");
    str_appendz(&s, "}");
    Json *j = json_parsez(s.data); str_free(&s); return j;
}
static void receive(IssuePages *p, const int *numbers, size_t count, int next) {
    Json *j = page(numbers, count, next, false); CHECK(issue_pages_receive(p, j)); json_free(j);
}
static size_t range(int *into, int from, int to) { size_t n = 0; for (int k = from; k <= to; k++) into[n++] = k; return n; }
static int offset_of(const IssuePages *p, const Json *b) {
    Json *q = issue_pages_query(p, "o/r", b); int offset = json_int_or(json_get(q, "offset"), -1); json_free(q); return offset;
}
/// The numbers the Issues tab lists for this board, the board's rows first.
static void check_listed(const IssuePages *p, const Json *b, const int *expected, size_t count) {
    size_t bn; IssueSummary *rows = issue_summaries_parse(json_get(b, "issues"), &bn);
    size_t n; IssueSummary *listed = issue_pages_listed(p, b, rows, bn, &n);
    CHECK_INT(n, count);
    for (size_t i = 0; i < n && i < count; i++) CHECK_INT(listed[i].number, expected[i]);
    free(listed); issue_summaries_free(rows, bn);
}

static void test_offered_only_when_the_board_stopped_short_and_could_read_issues(void) {
    IssuePages p = {0};
    int some[] = { 3, 2 };
    Json *cut = board(some, 2, true, NULL), *whole = board(some, 2, false, NULL), *refused = board(NULL, 0, true, "Resource not accessible");
    CHECK(issue_pages_can_load_more(&p, cut, true));
    CHECK(!issue_pages_can_load_more(&p, whole, true));
    CHECK(!issue_pages_can_load_more(&p, refused, true));
    CHECK(!issue_pages_can_load_more(&p, NULL, true));
    // Only a whole board says an issue missing from it is gone.
    CHECK(board_lists_every_issue(whole)); CHECK(!board_lists_every_issue(cut)); CHECK(!board_lists_every_issue(refused));
    Json *refused_whole = board(NULL, 0, false, "Resource not accessible"); CHECK(!board_lists_every_issue(refused_whole));
    json_free(cut); json_free(whole); json_free(refused); json_free(refused_whole);
}
static void test_a_server_without_the_route_says_the_list_is_cut_instead(void) {
    IssuePages p = {0};
    int some[] = { 3, 2 };
    Json *cut = board(some, 2, true, NULL), *whole = board(some, 2, false, NULL);
    CHECK(!issue_pages_can_load_more(&p, cut, false));
    CHECK(issue_pages_cut_short(&p, cut, false));
    CHECK(!issue_pages_cut_short(&p, cut, true));
    CHECK(!issue_pages_cut_short(&p, whole, false));
    json_free(cut); json_free(whole);
}
static void test_pages_that_end_with_nothing_new_leave_the_list_saying_it_is_cut(void) {
    int some[] = { 3, 2 }, on_board[] = { 2 }, new_one[] = { 1 };
    Json *cut = board(some, 2, true, NULL);
    IssuePages p = {0};
    receive(&p, on_board, 1, -1);
    CHECK(!issue_pages_can_load_more(&p, cut, true));
    CHECK(issue_pages_cut_short(&p, cut, true));
    // Pages that added issues read to the end: nothing is cut.
    IssuePages read = {0};
    receive(&read, new_one, 1, -1);
    CHECK(!issue_pages_cut_short(&read, cut, true));
    issue_pages_clear(&p); issue_pages_clear(&read); json_free(cut);
}
static void test_a_mirror_that_stopped_short_leaves_the_list_saying_it_is_cut(void) {
    int some[] = { 3, 2 }, new_one[] = { 1 };
    Json *cut = board(some, 2, true, NULL), *answer = page(new_one, 1, -1, true);
    IssuePages p = {0};
    CHECK(issue_pages_receive(&p, answer));
    CHECK(!issue_pages_can_load_more(&p, cut, true));
    CHECK(issue_pages_cut_short(&p, cut, true));
    issue_pages_clear(&p); json_free(cut); json_free(answer);
}
static void test_first_page_starts_after_the_board_then_follows_next_offset(void) {
    int some[] = { 9, 8, 7 }, next[] = { 6, 5 };
    Json *b = board(some, 3, true, NULL);
    IssuePages p = {0};
    Json *q = issue_pages_query(&p, "o/r", b);
    CHECK_STR(json_str(json_get(q, "repo")), "o/r");
    CHECK_INT(json_int_or(json_get(q, "offset"), -1), 0);
    CHECK_INT(json_int_or(json_get(q, "limit"), -1), 50);
    json_free(q);
    receive(&p, next, 2, 53);
    CHECK_INT(offset_of(&p, b), 43);
    CHECK(issue_pages_can_load_more(&p, b, true));
    issue_pages_clear(&p); json_free(b);
}
static void test_the_next_page_reaches_only_the_overlap_inside_the_boards_rows(void) {
    int numbers[160], seven[] = { 7 }, few[] = { 9, 8 };
    IssuePages p = {0};
    receive(&p, seven, 1, 30);
    // A newer board lists more than the page said to skip.
    size_t listed = range(numbers, 1, 50);
    Json *newer = board(numbers, listed, true, NULL);
    CHECK_INT(offset_of(&p, newer), 40);
    issue_pages_clear(&p);
    Json *small = board(few, 2, true, NULL);
    CHECK_INT(offset_of(&p, small), 0);
    json_free(newer); json_free(small);
}
static void test_an_issue_a_newer_board_pushes_past_its_edge_stays_listed(void) {
    int numbers[160], expected[160];
    // The board lists 2...101; the first page reaches back over its last rows.
    size_t listed = range(numbers, 2, 101);
    Json *first = board(numbers, listed, true, NULL);
    IssuePages p = {0};
    CHECK_INT(offset_of(&p, first), 90);
    listed = range(numbers, 92, 141);
    receive(&p, numbers, listed, 140);
    // A new issue 1 pushes 101 off a fresh board, which the pages are not reset for.
    listed = range(numbers, 1, 100);
    Json *newer = board(numbers, listed, true, NULL);
    size_t wanted = range(expected, 1, 141);
    check_listed(&p, newer, expected, wanted);
    CHECK_INT(offset_of(&p, newer), 130);
    issue_pages_clear(&p); json_free(first); json_free(newer);
}
static void test_keeps_each_issue_once_and_ends_with_the_last_page(void) {
    int some[] = { 9, 8 }, first[] = { 8, 7, 6 }, second[] = { 6, 5 }, expected[] = { 9, 8, 7, 6, 5 };
    Json *b = board(some, 2, true, NULL);
    IssuePages p = {0};
    // The list moved: 8 is on the board and 6 came back on the second page.
    receive(&p, first, 3, 5);
    receive(&p, second, 2, -1);
    CHECK_INT(p.count, 4);
    for (size_t i = 0; i < p.count && i < 4; i++) CHECK_INT(p.issues[i].number, first[0] - (int)i);
    check_listed(&p, b, expected, 5);
    CHECK(p.ended);
    CHECK(!issue_pages_can_load_more(&p, b, true));
    issue_pages_clear(&p); json_free(b);
}
static void test_lists_the_boards_issues_then_those_read_past_them(void) {
    int some[] = { 9, 8 }, read[] = { 7, 6 }, moved[] = { 7, 9, 8 };
    int expected[] = { 9, 8, 7, 6 }, expected_moved[] = { 7, 9, 8, 6 };
    Json *b = board(some, 2, true, NULL), *newer = board(moved, 3, true, NULL), *whole = board(some, 2, false, NULL);
    IssuePages p = {0};
    receive(&p, read, 2, 4);
    check_listed(&p, b, expected, 4);
    // A newer board listing 7 itself keeps it in the board's place.
    check_listed(&p, newer, expected_moved, 4);
    // A board no longer cut short lists every open issue itself.
    check_listed(&p, whole, some, 2);
    issue_pages_clear(&p); json_free(b); json_free(newer); json_free(whole);
}
static void test_an_issue_closed_from_a_loaded_page_is_no_longer_listed(void) {
    int some[] = { 3, 2 }, read[] = { 5, 4 }, expected[] = { 3, 2, 4 };
    Json *cut = board(some, 2, true, NULL);
    IssuePages p = {0};
    receive(&p, read, 2, 60);
    issue_pages_drop(&p, 5);
    check_listed(&p, cut, expected, 3);
    issue_pages_drop(&p, 42);
    CHECK_INT(p.count, 1);
    issue_pages_clear(&p); json_free(cut);
}
static void test_a_failed_page_is_read_again_from_the_same_offset(void) {
    int some[] = { 9 }, read[] = { 8 };
    Json *b = board(some, 1, true, NULL), *whole = board(some, 1, false, NULL);
    IssuePages p = {0};
    receive(&p, read, 1, 60);
    issue_pages_fail(&p, "offline");
    CHECK_STR(p.error, "offline");
    CHECK(issue_pages_retries(&p));
    CHECK(issue_pages_can_load_more(&p, b, true));
    CHECK_INT(offset_of(&p, b), 50);
    // The error shows only while Load more can read it again.
    CHECK_STR(issue_pages_retry_error(&p, b, true), "offline");
    CHECK(issue_pages_retry_error(&p, whole, true) == NULL);
    CHECK(issue_pages_retry_error(&p, b, false) == NULL);
    issue_pages_reading(&p);
    CHECK(p.error == NULL);
    CHECK(!issue_pages_retries(&p));
    issue_pages_clear(&p); json_free(b); json_free(whole);
}
static void test_reads_a_page_answer_and_refuses_one_that_is_not(void) {
    IssuePages p = {0};
    Json *j = json_parsez("{\"repo\":\"o/r\",\"issues\":[{\"number\":4},{\"number\":3},{\"number\":0},{\"title\":\"No number\"}],\"total\":120,\"nextOffset\":null}");
    CHECK(issue_pages_receive(&p, j)); json_free(j);
    CHECK_INT(p.count, 2); CHECK(p.ended); CHECK(!p.mirror_cut);
    CHECK_STR(p.issues[0].title, "Issue #4");
    issue_pages_clear(&p);
    j = json_parsez("{\"issues\":[],\"nextOffset\":150}"); CHECK(issue_pages_receive(&p, j)); json_free(j);
    CHECK(!p.ended); CHECK_INT(p.next_offset, 150);
    // Not a page: nothing changes and the read is reported as failed.
    const char *bad[] = { "{}", "{\"issues\":{}}", "{\"issues\":[],\"nextOffset\":\"next\"}", "{\"issues\":[],\"nextOffset\":-5}", "[]" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        j = json_parsez(bad[i]); CHECK(!issue_pages_receive(&p, j)); json_free(j);
        CHECK_INT(p.next_offset, 150); CHECK(!p.ended);
    }
    CHECK(!issue_pages_receive(&p, NULL));
    issue_pages_clear(&p);
    CHECK(p.issues == NULL); CHECK_INT(p.count, 0); CHECK(!p.answered);
}

void core_issue_pages_tests(void) {
    test_run("issue pages are offered only when the board stopped short and could read issues", test_offered_only_when_the_board_stopped_short_and_could_read_issues);
    test_run("issue pages: a server without the route says the list is cut", test_a_server_without_the_route_says_the_list_is_cut_instead);
    test_run("issue pages that end with nothing new leave the list cut", test_pages_that_end_with_nothing_new_leave_the_list_saying_it_is_cut);
    test_run("issue pages: a mirror that stopped short leaves the list cut", test_a_mirror_that_stopped_short_leaves_the_list_saying_it_is_cut);
    test_run("issue pages start after the board then follow nextOffset", test_first_page_starts_after_the_board_then_follows_next_offset);
    test_run("issue pages reach only the overlap inside the board's rows", test_the_next_page_reaches_only_the_overlap_inside_the_boards_rows);
    test_run("issue pages keep an issue a newer board pushes past its edge", test_an_issue_a_newer_board_pushes_past_its_edge_stays_listed);
    test_run("issue pages keep each issue once and end with the last page", test_keeps_each_issue_once_and_ends_with_the_last_page);
    test_run("issue pages list the board's issues then those read past them", test_lists_the_boards_issues_then_those_read_past_them);
    test_run("issue pages drop an issue closed in the app", test_an_issue_closed_from_a_loaded_page_is_no_longer_listed);
    test_run("issue pages read a failed page again from the same offset", test_a_failed_page_is_read_again_from_the_same_offset);
    test_run("issue pages read a page answer and refuse one that is not", test_reads_a_page_answer_and_refuses_one_that_is_not);
}
