// The board: labels, links, pull request and issue rows, filters, errands, merge warnings, reviews, stacks and ▶ Run.
#include "board.h"
#include "json.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool pull_of(const char *text, PullSummary *out) {
    Json *j = json_parsez(text);
    bool ok = pull_summary_parse(j, out);
    json_free(j);
    return ok;
}
static bool issue_of(const char *text, IssueSummary *out) {
    Json *j = json_parsez(text);
    bool ok = issue_summary_parse(j, out);
    json_free(j);
    return ok;
}
static bool link_of(const char *text, BoardLink *out) {
    Json *j = json_parsez(text);
    bool ok = board_link_parse(j, out);
    json_free(j);
    return ok;
}
static bool label_of(const char *text, PullLabel *out) {
    Json *j = json_parsez(text);
    bool ok = pull_label_parse(j, out);
    json_free(j);
    return ok;
}
static bool rgb_of(const char *color, int rgb[3]) {
    PullLabel l = { "x", (char *)color };
    return pull_label_rgb(&l, rgb);
}

// MARK: - Labels

static void test_labels_read_from_an_object_or_a_bare_name(void) {
    PullLabel l;
    CHECK(label_of("{\"name\":\"bug\",\"color\":\"d73a4a\"}", &l)); CHECK_STR(l.name, "bug"); CHECK_STR(l.color, "d73a4a"); pull_label_free(&l);
    CHECK(l.name == NULL && l.color == NULL);
    CHECK(label_of("\"backend\"", &l)); CHECK_STR(l.name, "backend"); CHECK(l.color == NULL); pull_label_free(&l);
    CHECK(label_of("{\"name\":\"x\",\"color\":7}", &l)); CHECK(l.color == NULL); pull_label_free(&l);
    CHECK(!label_of("{\"color\":\"ffffff\"}", &l)); CHECK(l.name == NULL);
    CHECK(!label_of("{\"name\":\"\"}", &l));
    CHECK(!label_of("\"\"", &l));
    CHECK(!label_of("{\"name\":3}", &l));
    CHECK(!label_of("12", &l));
    CHECK(!label_of("null", &l));
    CHECK(!pull_label_parse(NULL, &l));
    pull_label_free(NULL);
}
static void test_label_colours_read_as_six_hex_digits(void) {
    int rgb[3] = { -1, -1, -1 };
    CHECK(rgb_of("d93f0b", rgb)); CHECK_INT(rgb[0], 0xd9); CHECK_INT(rgb[1], 0x3f); CHECK_INT(rgb[2], 0x0b);
    CHECK(rgb_of("D93F0B", rgb)); CHECK_INT(rgb[0], 0xd9); CHECK_INT(rgb[1], 0x3f); CHECK_INT(rgb[2], 0x0b);
    CHECK(rgb_of("000000", rgb)); CHECK(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 0);
    CHECK(rgb_of("ffffff", rgb)); CHECK(rgb[0] == 255 && rgb[1] == 255 && rgb[2] == 255);
    CHECK(rgb_of("0a0B0c", rgb)); CHECK(rgb[0] == 10 && rgb[1] == 11 && rgb[2] == 12);
    // GitHub sends its colours bare; anything else is not read, and leaves the caller's colour alone.
    rgb[0] = rgb[1] = rgb[2] = 7;
    CHECK(!rgb_of("#d93f0b", rgb));
    CHECK(!rgb_of("fff", rgb));
    CHECK(!rgb_of("#fff", rgb));
    CHECK(!rgb_of("gggggg", rgb));
    CHECK(!rgb_of("12345z", rgb));
    CHECK(!rgb_of("d93f0b0", rgb));
    CHECK(!rgb_of("d93f0", rgb));
    CHECK(!rgb_of("", rgb));
    CHECK(!rgb_of(" d93f0", rgb));
    CHECK(!rgb_of(NULL, rgb));
    CHECK(rgb[0] == 7 && rgb[1] == 7 && rgb[2] == 7);
}

// MARK: - Links

static void test_board_links_need_a_number_and_default_their_title(void) {
    BoardLink l;
    CHECK(link_of("{\"number\":4,\"title\":\"Login\",\"url\":\"https://github.com/o/r/issues/4\",\"repo\":\"o/r\",\"draft\":true,"
                  "\"state\":\"open\",\"stateReason\":null,\"labels\":[{\"name\":\"bug\"},\"ui\",{\"color\":\"fff\"}]}", &l));
    CHECK_INT(l.number, 4); CHECK_STR(l.title, "Login"); CHECK_STR(l.url, "https://github.com/o/r/issues/4"); CHECK_STR(l.repo, "o/r");
    CHECK(l.draft); CHECK_STR(l.state, "open"); CHECK(l.state_reason == NULL);
    CHECK_INT(l.label_count, 2); CHECK_STR(l.labels[0].name, "bug"); CHECK_STR(l.labels[1].name, "ui");
    board_link_free(&l); CHECK(l.title == NULL && l.labels == NULL && l.label_count == 0);
    CHECK(link_of("{\"number\":9}", &l));
    CHECK_STR(l.title, "#9"); CHECK(l.url == NULL && l.repo == NULL && l.state == NULL && !l.draft && l.label_count == 0);
    board_link_free(&l);
    CHECK(link_of("{\"number\":9,\"draft\":\"true\",\"title\":5}", &l)); CHECK(!l.draft); CHECK_STR(l.title, "#9"); board_link_free(&l);
    CHECK(!link_of("{\"title\":\"No number\"}", &l));
    CHECK(!link_of("{\"number\":\"4\"}", &l));
    CHECK(!link_of("[]", &l));
    CHECK(!board_link_parse(NULL, &l));
    board_link_free(NULL);
}
static void test_board_links_name_their_repository_only_when_foreign(void) {
    BoardLink l;
    CHECK(link_of("{\"number\":3,\"repo\":\"acme/other\"}", &l));
    CHECK(board_link_is_foreign(&l, "o/r"));
    CHECK_OWNED_STR(board_link_reference(&l, "o/r"), "acme/other#3");
    CHECK(!board_link_is_foreign(&l, "Acme/Other"));
    CHECK_OWNED_STR(board_link_reference(&l, "ACME/OTHER"), "#3");
    CHECK(board_link_is_foreign(&l, NULL));
    CHECK_OWNED_STR(board_link_reference(&l, NULL), "acme/other#3");
    board_link_free(&l);
    // A link that names no repository is this one's.
    CHECK(link_of("{\"number\":3}", &l));
    CHECK(!board_link_is_foreign(&l, "o/r")); CHECK(!board_link_is_foreign(&l, NULL));
    CHECK_OWNED_STR(board_link_reference(&l, "o/r"), "#3");
    board_link_free(&l);
}
static void test_board_links_are_not_planned_only_when_closed_as_such(void) {
    const char *cases[] = {
        "{\"number\":1,\"state\":\"closed\",\"stateReason\":\"not_planned\"}",
        "{\"number\":1,\"state\":\"closed\",\"stateReason\":\"completed\"}",
        "{\"number\":1,\"state\":\"closed\"}",
        "{\"number\":1,\"state\":\"open\",\"stateReason\":\"not_planned\"}",
        "{\"number\":1,\"stateReason\":\"not_planned\"}",
        "{\"number\":1,\"state\":\"CLOSED\",\"stateReason\":\"NOT_PLANNED\"}",
    };
    const bool expected[] = { true, false, false, false, false, false };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        BoardLink l; CHECK(link_of(cases[i], &l));
        CHECK(board_link_not_planned(&l) == expected[i]);
        board_link_free(&l);
    }
}
static void test_board_link_copies_are_independent(void) {
    BoardLink from, into;
    CHECK(link_of("{\"number\":5,\"title\":\"T\",\"url\":\"https://x\",\"repo\":\"a/b\",\"draft\":true,\"state\":\"closed\",\"stateReason\":\"not_planned\","
                  "\"labels\":[{\"name\":\"bug\",\"color\":\"ff0000\"}]}", &from));
    board_link_copy(&into, &from);
    board_link_free(&from);
    CHECK_INT(into.number, 5); CHECK_STR(into.title, "T"); CHECK_STR(into.url, "https://x"); CHECK_STR(into.repo, "a/b");
    CHECK(into.draft); CHECK(board_link_not_planned(&into));
    CHECK_INT(into.label_count, 1); CHECK_STR(into.labels[0].name, "bug"); CHECK_STR(into.labels[0].color, "ff0000");
    board_link_free(&into);
    CHECK(link_of("{\"number\":6}", &from));
    board_link_copy(&into, &from);
    CHECK_STR(into.title, "#6"); CHECK(into.url == NULL && into.repo == NULL && into.state == NULL && into.state_reason == NULL && into.label_count == 0);
    board_link_free(&into); board_link_free(&from);
}

// MARK: - Pull requests

static void test_pull_summaries_read_every_field(void) {
    PullSummary p;
    CHECK(pull_of("{\"number\":7,\"title\":\"Add invoices\",\"url\":\"https://github.com/o/r/pull/7\",\"branch\":\"feat/x\",\"baseBranch\":\"main\",\"draft\":true,"
                  "\"author\":\"Ana\",\"assignees\":[\"ana\",3,\"luis\"],"
                  "\"reviewers\":[{\"user\":\"luis\",\"state\":\"APPROVED\"},{\"state\":\"approved\"},{\"user\":\"bo\"},\"x\"],"
                  "\"labels\":[{\"name\":\"bug\",\"color\":\"d73a4a\"},\"ui\",{}],"
                  "\"issues\":[{\"number\":3,\"repo\":\"o/r\"},{\"title\":\"none\"}],"
                  "\"mergeable\":\"mergeable\",\"checks\":\"success\",\"reviewDecision\":\"APPROVED\",\"recommended\":\"run\","
                  "\"updatedAt\":\"2026-09-28T15:55:49Z\"}", &p));
    CHECK_INT(p.number, 7); CHECK_STR(p.title, "Add invoices"); CHECK_STR(p.url, "https://github.com/o/r/pull/7");
    CHECK_STR(p.branch, "feat/x"); CHECK_STR(p.base_branch, "main"); CHECK(p.draft); CHECK_STR(p.author, "Ana");
    CHECK_INT(p.assignee_count, 2); CHECK_STR(p.assignees[0], "ana"); CHECK_STR(p.assignees[1], "luis");
    CHECK_INT(p.reviewer_count, 2); CHECK_STR(p.reviewers[0].user, "luis"); CHECK_STR(p.reviewers[0].state, "APPROVED");
    CHECK_STR(p.reviewers[1].user, "bo"); CHECK_STR(p.reviewers[1].state, "");
    CHECK_INT(p.label_count, 2); CHECK_STR(p.labels[1].name, "ui");
    CHECK_INT(p.issue_count, 1); CHECK_INT(p.issues[0].number, 3);
    CHECK_STR(p.mergeable, "mergeable"); CHECK_STR(p.checks, "success"); CHECK_STR(p.review_decision, "APPROVED"); CHECK_STR(p.recommended, "run");
    CHECK(p.has_updated);
    CHECK(json_num_or(json_get(p.raw, "number"), 0) == 7); CHECK_STR(json_str(json_get(p.raw, "title")), "Add invoices");
    pull_summary_free(&p); CHECK(p.raw == NULL && p.title == NULL && p.reviewer_count == 0);
    pull_summary_free(NULL);
}
static void test_pull_summaries_default_what_the_server_left_out(void) {
    PullSummary p;
    CHECK(pull_of("{\"number\":12}", &p));
    CHECK_STR(p.title, "Pull request #12");
    CHECK(p.url == NULL && p.author == NULL && p.checks == NULL && p.review_decision == NULL && p.recommended == NULL);
    // Branches are never NULL, and GitHub's merge answer is unknown until it says.
    CHECK_STR(p.branch, ""); CHECK_STR(p.base_branch, ""); CHECK_STR(p.mergeable, "unknown");
    CHECK(!p.draft && !p.has_updated);
    CHECK(p.assignee_count == 0 && p.reviewer_count == 0 && p.label_count == 0 && p.issue_count == 0);
    CHECK(!pull_conflicting(&p) && !pull_has_conflicts(&p) && !pull_checks_failed(&p) && !pull_awaits_feedback(&p));
    pull_summary_free(&p);
    CHECK(pull_of("{\"number\":12,\"title\":null,\"branch\":4,\"draft\":\"true\",\"mergeable\":false,\"updatedAt\":\"yesterday\",\"reviewers\":{\"user\":\"x\"}}", &p));
    CHECK_STR(p.title, "Pull request #12"); CHECK_STR(p.branch, ""); CHECK(!p.draft); CHECK_STR(p.mergeable, "unknown"); CHECK(!p.has_updated);
    CHECK_INT(p.reviewer_count, 0);
    pull_summary_free(&p);
    CHECK(pull_of("{\"number\":7.9}", &p)); CHECK_INT(p.number, 7); pull_summary_free(&p);
}
static void test_pull_summaries_without_a_positive_number_are_rejected(void) {
    PullSummary p;
    CHECK(!pull_of("{\"title\":\"x\"}", &p)); CHECK(p.raw == NULL);
    CHECK(!pull_of("{\"number\":0}", &p));
    CHECK(!pull_of("{\"number\":-3}", &p));
    CHECK(!pull_of("{\"number\":\"7\"}", &p));
    CHECK(!pull_of("{\"number\":null}", &p));
    CHECK(!pull_of("[7]", &p));
    CHECK(!pull_summary_parse(NULL, &p));
}
static void test_pull_summary_lists_keep_only_valid_rows(void) {
    Json *j = json_parsez("[{\"number\":1},{\"number\":0},\"x\",null,{\"title\":\"t\"},{\"number\":2},3]");
    size_t n = 99; PullSummary *p = pull_summaries_parse(j, &n);
    CHECK_INT(n, 2); if (n == 2) { CHECK_INT(p[0].number, 1); CHECK_INT(p[1].number, 2); }
    pull_summaries_free(p, n); json_free(j);
    j = json_parsez("[]"); p = pull_summaries_parse(j, &n); CHECK_INT(n, 0); pull_summaries_free(p, n); json_free(j);
    j = json_parsez("{\"number\":1}"); p = pull_summaries_parse(j, &n); CHECK_INT(n, 0); pull_summaries_free(p, n); json_free(j);
    n = 99; p = pull_summaries_parse(NULL, &n); CHECK_INT(n, 0); pull_summaries_free(p, n);
}
static void test_pull_summary_copies_are_independent(void) {
    PullSummary from, into;
    CHECK(pull_of("{\"number\":7,\"title\":\"T\",\"branch\":\"b\",\"baseBranch\":\"main\",\"author\":\"ana\",\"assignees\":[\"x\"],"
                  "\"reviewers\":[{\"user\":\"luis\",\"state\":\"approved\"}],\"labels\":[{\"name\":\"has-conflicts\"}],"
                  "\"issues\":[{\"number\":3}],\"mergeable\":\"conflicting\",\"checks\":\"error\",\"draft\":true}", &from));
    pull_summary_copy(&into, &from);
    CHECK(into.raw != from.raw);
    pull_summary_free(&from);
    CHECK_INT(into.number, 7); CHECK_STR(into.title, "T"); CHECK_STR(into.branch, "b"); CHECK_STR(into.base_branch, "main");
    CHECK_STR(into.author, "ana"); CHECK(into.draft);
    CHECK_INT(into.assignee_count, 1); CHECK_INT(into.reviewer_count, 1); CHECK_STR(into.reviewers[0].user, "luis");
    CHECK_INT(into.label_count, 1); CHECK_INT(into.issue_count, 1);
    CHECK(pull_conflicting(&into) && pull_has_conflicts(&into) && pull_checks_failed(&into));
    CHECK_STR(json_str(json_get(into.raw, "branch")), "b");
    pull_summary_free(&into);
}
static void test_pull_predicates_read_mergeable_checks_and_labels(void) {
    struct { const char *json; bool conflicting, conflicts, failed, feedback; } cases[] = {
        { "{\"number\":1,\"mergeable\":\"conflicting\"}", true, true, false, false },
        { "{\"number\":1,\"mergeable\":\"CONFLICTING\"}", false, false, false, false },
        { "{\"number\":1,\"mergeable\":\"mergeable\"}", false, false, false, false },
        { "{\"number\":1,\"mergeable\":\"unknown\",\"labels\":[\"HAS-CONFLICTS\"]}", false, true, false, false },
        { "{\"number\":1,\"labels\":[{\"name\":\"has-conflicts-maybe\"}]}", false, false, false, false },
        { "{\"number\":1,\"checks\":\"failure\"}", false, false, true, false },
        { "{\"number\":1,\"checks\":\"error\"}", false, false, true, false },
        { "{\"number\":1,\"checks\":\"pending\"}", false, false, false, false },
        { "{\"number\":1,\"checks\":\"expected\"}", false, false, false, false },
        { "{\"number\":1,\"checks\":\"success\"}", false, false, false, false },
        { "{\"number\":1,\"checks\":\"FAILURE\"}", false, false, false, false },
        { "{\"number\":1,\"labels\":[\"Feedback-Given\"]}", false, false, false, true },
        { "{\"number\":1,\"labels\":[\"feedback\"]}", false, false, false, false },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        PullSummary p; CHECK(pull_of(cases[i].json, &p));
        CHECK(pull_conflicting(&p) == cases[i].conflicting);
        CHECK(pull_has_conflicts(&p) == cases[i].conflicts);
        CHECK(pull_checks_failed(&p) == cases[i].failed);
        CHECK(pull_awaits_feedback(&p) == cases[i].feedback);
        pull_summary_free(&p);
    }
}

// MARK: - Issues

static void test_issue_summaries_read_every_field(void) {
    IssueSummary i;
    CHECK(issue_of("{\"number\":20,\"title\":\"Child\",\"url\":\"https://github.com/o/r/issues/20\",\"author\":\"ana\",\"milestone\":\"v2\","
                   "\"assignees\":[\"luis\"],\"labels\":[\"bug\"],\"comments\":4,\"updatedAt\":\"2026-09-28T15:55:49Z\","
                   "\"parent\":{\"number\":21,\"title\":\"Epic\",\"repo\":\"o/r\"},\"subIssues\":{\"total\":2,\"completed\":1},"
                   "\"pulls\":[{\"number\":8,\"draft\":true},{\"title\":\"no number\"}]}", &i));
    CHECK_INT(i.number, 20); CHECK_STR(i.title, "Child"); CHECK_STR(i.url, "https://github.com/o/r/issues/20"); CHECK_STR(i.author, "ana");
    CHECK_STR(i.milestone, "v2"); CHECK_INT(i.assignee_count, 1); CHECK_INT(i.label_count, 1); CHECK_INT(i.comments, 4); CHECK(i.has_updated);
    CHECK(i.has_parent); CHECK_INT(i.parent.number, 21); CHECK_STR(i.parent.title, "Epic");
    CHECK_INT(i.sub_issues, 2); CHECK_INT(i.sub_issues_done, 1); CHECK(issue_is_epic(&i));
    CHECK_INT(i.pull_count, 1); CHECK(i.pulls[0].draft);
    issue_summary_free(&i); CHECK(i.title == NULL && !i.has_parent);
    issue_summary_free(NULL);
}
static void test_issue_summaries_default_what_the_server_left_out(void) {
    IssueSummary i;
    CHECK(issue_of("{\"number\":5}", &i));
    CHECK_STR(i.title, "Issue #5"); CHECK(i.url == NULL && i.author == NULL && i.milestone == NULL);
    CHECK(i.comments == 0 && !i.has_updated && !i.has_parent && i.sub_issues == 0 && i.sub_issues_done == 0);
    CHECK(i.assignee_count == 0 && i.label_count == 0 && i.pull_count == 0 && !issue_is_epic(&i));
    issue_summary_free(&i);
    // A parent without a number is no parent; counts of the wrong type read as none.
    CHECK(issue_of("{\"number\":5,\"parent\":{\"title\":\"Epic\"},\"comments\":\"4\",\"subIssues\":{\"total\":\"3\"}}", &i));
    CHECK(!i.has_parent && i.comments == 0 && i.sub_issues == 0);
    issue_summary_free(&i);
    CHECK(issue_of("{\"number\":5,\"parent\":null,\"subIssues\":{\"total\":0,\"completed\":0}}", &i));
    CHECK(!i.has_parent && !issue_is_epic(&i));
    issue_summary_free(&i);
    CHECK(issue_of("{\"number\":5,\"parent\":{\"number\":6}}", &i)); CHECK(i.has_parent); CHECK_STR(i.parent.title, "#6"); issue_summary_free(&i);
    CHECK(!issue_of("{\"title\":\"x\"}", &i));
    CHECK(!issue_of("{\"number\":0}", &i));
    CHECK(!issue_of("{\"number\":-1}", &i));
    CHECK(!issue_of("{\"number\":\"5\"}", &i));
    Json *j = json_parsez("[{\"number\":1},{\"number\":0},\"x\",{\"number\":2}]");
    size_t n; IssueSummary *list = issue_summaries_parse(j, &n);
    CHECK_INT(n, 2); if (n == 2) CHECK_INT(list[1].number, 2);
    issue_summaries_free(list, n); json_free(j);
    list = issue_summaries_parse(NULL, &n); CHECK_INT(n, 0); issue_summaries_free(list, n);
}
static char *nested_text(const char *json, const char *repo) {
    Json *j = json_parsez(json);
    size_t n; IssueSummary *issues = issue_summaries_parse(j, &n);
    size_t rn; IssueRow *rows = issues_nested(issues, n, repo, &rn);
    Str s; str_init(&s);
    for (size_t i = 0; i < rn; i++) str_appendf(&s, "%s%d:%d", i ? "," : "", issues[rows[i].index].number, rows[i].depth);
    if (rn != n) str_appendf(&s, " (%d rows for %d issues)", (int)rn, (int)n);
    free(rows); issue_summaries_free(issues, n); json_free(j);
    return s.data ? str_detach(&s) : xstrdup("");
}
static void test_issues_nest_depth_first_under_epics_on_the_list(void) {
    // Children listed before their epic still follow it, in list order, however deep.
    CHECK_OWNED_STR(nested_text("[{\"number\":3,\"parent\":{\"number\":2}},{\"number\":4,\"parent\":{\"number\":1}},{\"number\":1},"
                                "{\"number\":2,\"parent\":{\"number\":1}},{\"number\":5}]", "o/r"),
                    "1:0,4:1,2:1,3:2,5:0");
    // The parent named in this repository's other case is still this one's.
    CHECK_OWNED_STR(nested_text("[{\"number\":1},{\"number\":2,\"parent\":{\"number\":1,\"repo\":\"O/R\"}}]", "o/r"), "1:0,2:1");
    // A parent elsewhere, one not on the list, and an issue its own parent stay flat.
    CHECK_OWNED_STR(nested_text("[{\"number\":1},{\"number\":2,\"parent\":{\"number\":1,\"repo\":\"acme/other\"}},"
                                "{\"number\":3,\"parent\":{\"number\":99}},{\"number\":4,\"parent\":{\"number\":4}}]", "o/r"),
                    "1:0,2:0,3:0,4:0");
    CHECK_OWNED_STR(nested_text("[]", "o/r"), "");
}
static void test_issues_in_a_cycle_are_each_drawn_once(void) {
    CHECK_OWNED_STR(nested_text("[{\"number\":1,\"parent\":{\"number\":3}},{\"number\":2,\"parent\":{\"number\":1}},{\"number\":3,\"parent\":{\"number\":2}}]", "o/r"),
                    "1:0,2:1,3:2");
    CHECK_OWNED_STR(nested_text("[{\"number\":9},{\"number\":1,\"parent\":{\"number\":2}},{\"number\":2,\"parent\":{\"number\":1}},"
                                "{\"number\":5,\"parent\":{\"number\":2}}]", "o/r"),
                    "9:0,1:0,2:1,5:2");
}
static void test_issue_prompts_name_the_issue_its_epic_and_how_to_close_it(void) {
    IssueSummary i;
    CHECK(issue_of("{\"number\":20,\"title\":\"Child\"}", &i));
    char *p = issue_prompt(&i, "o/r");
    CHECK(str_has_prefix(p, "Issue #20: Child\n\n"));
    CHECK(strstr(p, "Read o/r issue #20 in full") != NULL);
    CHECK(strstr(p, "`gh issue view 20 --repo o/r --comments`") != NULL);
    CHECK(strstr(p, "sub-issue") == NULL);
    CHECK(strstr(p, "this session\xE2\x80\x99s own branch") != NULL);
    CHECK(strstr(p, "`Closes #20`") != NULL);
    CHECK(str_has_suffix(p, "say what is missing and stop rather than guessing at it."));
    free(p); issue_summary_free(&i);
    CHECK(issue_of("{\"number\":5}", &i));
    p = issue_prompt(&i, "o/r"); CHECK(str_has_prefix(p, "Issue #5: Issue #5\n")); free(p); issue_summary_free(&i);
    // An epic named without a repository, or in this one's other case, is read here.
    CHECK(issue_of("{\"number\":20,\"title\":\"Child\",\"parent\":{\"number\":21}}", &i));
    p = issue_prompt(&i, "o/r"); CHECK(strstr(p, "It is a sub-issue of o/r#21 (#21). Read that epic too") != NULL); free(p); issue_summary_free(&i);
    CHECK(issue_of("{\"number\":20,\"title\":\"Child\",\"parent\":{\"number\":21,\"title\":\"Epic\",\"repo\":\"O/R\"}}", &i));
    p = issue_prompt(&i, "o/r"); CHECK(strstr(p, "sub-issue of o/r#21 (Epic)") != NULL); free(p); issue_summary_free(&i);
    CHECK(issue_of("{\"number\":20,\"title\":\"Child\",\"parent\":{\"number\":21,\"title\":\"Theirs\",\"repo\":\"acme/other\"}}", &i));
    p = issue_prompt(&i, "o/r"); CHECK(strstr(p, "sub-issue of acme/other#21 (Theirs)") != NULL); CHECK(strstr(p, "`Closes #20`") != NULL); free(p);
    issue_summary_free(&i);
}

// MARK: - Rows and filters

static void test_board_rows_carry_what_the_pickers_filter_on(void) {
    PullSummary p; IssueSummary i;
    CHECK(pull_of("{\"number\":1,\"author\":\"ana\",\"reviewers\":[{\"user\":\"luis\"}],\"labels\":[\"bug\",\"ui\"]}", &p));
    BoardRow r = pull_board_row(&p);
    CHECK(r.author == p.author && r.reviewers == p.reviewers && r.reviewer_count == 1 && r.labels == p.labels && r.label_count == 2);
    CHECK(issue_of("{\"number\":2,\"author\":\"bo\",\"assignees\":[\"x\"],\"labels\":[\"bug\"]}", &i));
    r = issue_board_row(&i);
    CHECK(r.author == i.author && r.reviewers == NULL && r.reviewer_count == 0 && r.labels == i.labels && r.label_count == 1);
    pull_summary_free(&p); issue_summary_free(&i);
}
static void test_board_filters_keep_their_picks_folded(void) {
    BoardFilter f; board_filter_init(&f);
    CHECK_STR(f.author, ""); CHECK_STR(f.reviewer, ""); CHECK_STR(f.label, ""); CHECK(!board_filter_is_on(&f));
    board_filter_set(&f, FILTER_AUTHOR, "TheBot"); CHECK_STR(board_filter_get(&f, FILTER_AUTHOR), "thebot"); CHECK(board_filter_is_on(&f));
    board_filter_set(&f, FILTER_REVIEWER, "Ana"); CHECK_STR(board_filter_get(&f, FILTER_REVIEWER), "ana");
    board_filter_set(&f, FILTER_LABEL, "Has-Conflicts"); CHECK_STR(board_filter_get(&f, FILTER_LABEL), "has-conflicts");
    board_filter_set(&f, FILTER_AUTHOR, NULL); CHECK_STR(f.author, "");
    board_filter_set(&f, FILTER_REVIEWER, ""); board_filter_set(&f, FILTER_LABEL, "");
    CHECK(!board_filter_is_on(&f));
    board_filter_set(&f, FILTER_LABEL, "x"); CHECK(board_filter_is_on(&f));
    CHECK_STR(filter_kind_name(FILTER_AUTHOR), "author"); CHECK_STR(filter_kind_name(FILTER_REVIEWER), "reviewer"); CHECK_STR(filter_kind_name(FILTER_LABEL), "label");
    board_filter_free(&f); CHECK(f.author == NULL && f.reviewer == NULL && f.label == NULL);
    board_filter_free(NULL);
}
static void test_board_filter_copies_compare_equal_and_are_independent(void) {
    BoardFilter a, b; board_filter_init(&a);
    board_filter_set(&a, FILTER_AUTHOR, "Ana"); board_filter_set(&a, FILTER_LABEL, "bug");
    board_filter_copy(&b, &a);
    CHECK(board_filter_equal(&a, &b)); CHECK(b.author != a.author);
    board_filter_set(&a, FILTER_AUTHOR, "luis");
    CHECK(!board_filter_equal(&a, &b)); CHECK_STR(b.author, "ana");
    board_filter_set(&b, FILTER_AUTHOR, "LUIS"); CHECK(board_filter_equal(&a, &b));
    board_filter_set(&b, FILTER_REVIEWER, "x"); CHECK(!board_filter_equal(&a, &b));
    board_filter_set(&b, FILTER_REVIEWER, ""); board_filter_set(&b, FILTER_LABEL, "ui"); CHECK(!board_filter_equal(&a, &b));
    board_filter_free(&a); board_filter_free(&b);
}
// Three pull requests and two issues the filter tests share.
static const char *const filter_pulls =
    "[{\"number\":1,\"author\":\"Ana\",\"reviewers\":[{\"user\":\"luis\"},{\"user\":\"Bo\"}],\"labels\":[\"bug\",\"UI\",\"Bug\"]},"
    "{\"number\":2,\"author\":\"luis\",\"reviewers\":[{\"user\":\"ana\"}],\"labels\":[\"backend\"]},"
    "{\"number\":3,\"author\":\"ana\",\"reviewers\":[{\"user\":\"LUIS\"}],\"labels\":[\"bug\"]},"
    "{\"number\":4,\"labels\":[\"zeta\",\"alpha\"]}]";
typedef struct { Json *json; PullSummary *pulls; size_t count; BoardRow rows[8]; } Rows;
static void rows_load(Rows *r, const char *json) {
    r->json = json_parsez(json); r->pulls = pull_summaries_parse(r->json, &r->count);
    for (size_t i = 0; i < r->count && i < 8; i++) r->rows[i] = pull_board_row(&r->pulls[i]);
}
static void rows_free(Rows *r) { pull_summaries_free(r->pulls, r->count); json_free(r->json); }
static char *passing_numbers(const BoardFilter *f, const Rows *r, int skipping) {
    Str s; str_init(&s);
    for (size_t i = 0; i < r->count; i++) if (board_filter_passes(f, &r->rows[i], skipping)) str_appendf(&s, "%s%d", s.len ? "," : "", r->pulls[i].number);
    return s.data ? str_detach(&s) : xstrdup("");
}
static char *options(const BoardFilter *f, FilterKind kind, const BoardRow *rows, size_t count) {
    size_t n; FilterOption *o = board_filter_options(f, kind, rows, count, &n);
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) str_appendf(&s, "%s%s=%s %d", i ? "," : "", o[i].value, o[i].text, o[i].count);
    filter_options_free(o, n);
    return s.data ? str_detach(&s) : xstrdup("");
}
static void test_board_filters_pass_rows_folding_case(void) {
    Rows r; rows_load(&r, filter_pulls);
    BoardFilter f; board_filter_init(&f);
    CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "1,2,3,4");
    board_filter_set(&f, FILTER_AUTHOR, "ANA"); CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "1,3");
    board_filter_set(&f, FILTER_REVIEWER, "luis"); CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "1,3");
    board_filter_set(&f, FILTER_LABEL, "ui"); CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "1");
    // Skipping a picker leaves it out of the test; the others still apply.
    CHECK_OWNED_STR(passing_numbers(&f, &r, FILTER_LABEL), "1,3");
    CHECK_OWNED_STR(passing_numbers(&f, &r, FILTER_AUTHOR), "1");
    board_filter_set(&f, FILTER_AUTHOR, "luis"); CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "");
    CHECK_OWNED_STR(passing_numbers(&f, &r, FILTER_AUTHOR), "1");
    // A row without an author never matches an author pick.
    board_filter_set(&f, FILTER_REVIEWER, ""); board_filter_set(&f, FILTER_LABEL, ""); board_filter_set(&f, FILTER_AUTHOR, "x");
    CHECK_OWNED_STR(passing_numbers(&f, &r, -1), "");
    board_filter_free(&f); rows_free(&r);
}
static void test_board_filter_options_are_counted_against_the_other_pickers(void) {
    Rows r; rows_load(&r, filter_pulls);
    BoardFilter f; board_filter_init(&f);
    // Case variants fold into one option named as first seen; a label twice on one row counts once; sorted ignoring case.
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, r.count), "ana=Ana 2,luis=luis 1");
    CHECK_OWNED_STR(options(&f, FILTER_REVIEWER, r.rows, r.count), "ana=ana 1,bo=Bo 1,luis=luis 2");
    CHECK_OWNED_STR(options(&f, FILTER_LABEL, r.rows, r.count), "alpha=alpha 1,backend=backend 1,bug=bug 2,ui=UI 1,zeta=zeta 1");
    board_filter_set(&f, FILTER_AUTHOR, "ana");
    // The author picker still counts every author; the others count only Ana's rows.
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, r.count), "ana=Ana 2,luis=luis 1");
    CHECK_OWNED_STR(options(&f, FILTER_LABEL, r.rows, r.count), "bug=bug 2,ui=UI 1");
    CHECK_OWNED_STR(options(&f, FILTER_REVIEWER, r.rows, r.count), "bo=Bo 1,luis=luis 2");
    board_filter_set(&f, FILTER_LABEL, "UI");
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, r.count), "ana=Ana 1");
    CHECK_OWNED_STR(options(&f, FILTER_REVIEWER, r.rows, r.count), "bo=Bo 1,luis=luis 1");
    board_filter_free(&f); rows_free(&r);
}
static void test_a_pick_the_others_empty_still_lists_itself(void) {
    Rows r; rows_load(&r, filter_pulls);
    BoardFilter f; board_filter_init(&f);
    board_filter_set(&f, FILTER_AUTHOR, "luis"); board_filter_set(&f, FILTER_LABEL, "UI");
    CHECK_OWNED_STR(options(&f, FILTER_LABEL, r.rows, r.count), "backend=backend 1,ui=ui 0");
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, r.count), "ana=Ana 1,luis=luis 0");
    // A pick nobody carries, or a board with no rows, lists only the pick.
    board_filter_set(&f, FILTER_AUTHOR, "Ghost");
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, 0), "ghost=ghost 0");
    board_filter_set(&f, FILTER_AUTHOR, ""); board_filter_set(&f, FILTER_LABEL, "");
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, r.rows, 0), "");
    size_t n = 99; FilterOption *o = board_filter_options(&f, FILTER_LABEL, NULL, 0, &n); CHECK_INT(n, 0); filter_options_free(o, n);
    filter_options_free(NULL, 0);
    board_filter_free(&f); rows_free(&r);
}
static void test_issue_rows_offer_no_reviewers(void) {
    Json *j = json_parsez("[{\"number\":1,\"author\":\"ana\",\"labels\":[\"bug\"]},{\"number\":2,\"author\":\"bo\"}]");
    size_t n; IssueSummary *issues = issue_summaries_parse(j, &n);
    BoardRow rows[2]; for (size_t i = 0; i < n; i++) rows[i] = issue_board_row(&issues[i]);
    BoardFilter f; board_filter_init(&f);
    CHECK_OWNED_STR(options(&f, FILTER_REVIEWER, rows, n), "");
    CHECK_OWNED_STR(options(&f, FILTER_AUTHOR, rows, n), "ana=ana 1,bo=bo 1");
    board_filter_set(&f, FILTER_REVIEWER, "luis");
    CHECK(!board_filter_passes(&f, &rows[0], -1)); CHECK(board_filter_passes(&f, &rows[0], FILTER_REVIEWER));
    CHECK_OWNED_STR(options(&f, FILTER_REVIEWER, rows, n), "luis=luis 0");
    board_filter_free(&f); issue_summaries_free(issues, n); json_free(j);
}
static void test_the_board_opens_on_the_author_only_while_they_have_rows(void) {
    Rows r; rows_load(&r, filter_pulls);
    BoardFilter f;
    board_filter_opening(&f, "ANA", r.rows, r.count); CHECK_STR(f.author, "ana"); CHECK_STR(f.reviewer, ""); CHECK_STR(f.label, ""); board_filter_free(&f);
    board_filter_opening(&f, "luis", r.rows, r.count); CHECK_STR(f.author, "luis"); board_filter_free(&f);
    // Reviewing is not authoring.
    board_filter_opening(&f, "Bo", r.rows, r.count); CHECK(!board_filter_is_on(&f)); CHECK_STR(f.author, ""); board_filter_free(&f);
    board_filter_opening(&f, "", r.rows, r.count); CHECK(!board_filter_is_on(&f)); board_filter_free(&f);
    board_filter_opening(&f, "ana", r.rows, 0); CHECK(!board_filter_is_on(&f)); CHECK_STR(f.author, ""); board_filter_free(&f);
    board_filter_opening(&f, "ana", NULL, 0); CHECK(!board_filter_is_on(&f)); board_filter_free(&f);
    rows_free(&r);
}

// MARK: - Actions

static char *action_ids(const BoardAction *a, size_t n) {
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) str_appendf(&s, "%s%s", i ? "," : "", a[i].id);
    return s.data ? str_detach(&s) : xstrdup("");
}
static char *offered(const char *catalog, const char *pull, int failed_checks) {
    Json *c = catalog ? json_parsez(catalog) : NULL;
    PullSummary p; bool has_pull = pull && pull_of(pull, &p);
    size_t n; BoardAction *a = board_actions_offered(c, has_pull ? &p : NULL, failed_checks, &n);
    char *ids = action_ids(a, n);
    board_actions_free(a, n);
    if (has_pull) pull_summary_free(&p);
    json_free(c);
    return ids;
}
static const BoardAction *known(const char *id) {
    size_t n; const BoardAction *k = board_actions_known(&n);
    for (size_t i = 0; i < n; i++) if (str_eq(k[i].id, id)) return &k[i];
    return NULL;
}
static void test_known_errands_are_listed_in_the_dashboards_order(void) {
    size_t n; const BoardAction *k = board_actions_known(&n);
    CHECK_OWNED_STR(action_ids(k, n), "run,review,solve-conflicts,fix-checks,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    for (size_t i = 0; i < n; i++) { CHECK(!str_empty(k[i].label)); CHECK(!str_empty(k[i].hint)); CHECK(k[i].has_input == str_eq(k[i].id, "custom-feedback")); }
    const BoardAction *feedback = known("custom-feedback");
    CHECK(feedback && feedback->input.required);
    if (feedback) { CHECK_STR(feedback->label, "Give feedback"); CHECK_STR(feedback->input.label, "Your feedback"); CHECK_STR(feedback->input.placeholder, "What should change on this pull request?"); }
    CHECK_STR(known("run")->label, "Run"); CHECK_STR(known("review")->label, "Code review");
    // QA and the test sheet were removed (#12).
    CHECK(known("qa") == NULL && known("test-sheet") == NULL && known("test-run") == NULL);
}
static void test_errands_start_through_their_own_operation(void) {
    size_t n; const BoardAction *k = board_actions_known(&n);
    // Since /api/v1 (#26) every errand but Run and Code review goes through `action`, not an operation named after it.
    for (size_t i = 0; i < n; i++) {
        const char *expected = str_eq(k[i].id, "run") ? "serve_pull" : str_eq(k[i].id, "review") ? "review" : "action";
        CHECK_OWNED_STR(board_action_operation(&k[i]), expected);
        CHECK_INT(board_action_timeout_ms(&k[i]), str_eq(k[i].id, "run") ? 170000 : 0);
    }
    BoardAction served = { "label-pull", "Label it", "", false, { NULL, NULL, false } };
    CHECK_OWNED_STR(board_action_operation(&served), "action"); CHECK_INT(board_action_timeout_ms(&served), 0);
}
static void test_errand_arguments_take_the_shape_each_route_wants(void) {
    Json *a = board_action_arguments(known("run"), "o/r", 9, "feat/x", "ignored");
    CHECK_INT(json_count(a), 2); CHECK_STR(json_str(json_get(a, "repo")), "o/r"); CHECK(json_num_or(json_get(a, "prNumber"), 0) == 9);
    CHECK(json_is_null(json_get(a, "branch"))); CHECK(json_is_null(json_get(a, "action"))); json_free(a);
    a = board_action_arguments(known("review"), "o/r", 9, "feat/x", NULL);
    CHECK_INT(json_count(a), 3); CHECK_STR(json_str(json_get(a, "branch")), "feat/x"); json_free(a);
    a = board_action_arguments(known("review"), "o/r", 9, NULL, NULL);
    CHECK_INT(json_count(a), 2); CHECK(json_is_null(json_get(a, "action"))); json_free(a);
    const char *errands[] = { "solve-conflicts", "fix-checks", "implement-feedback", "pr-body-summary", "delete-self-comments" };
    for (size_t i = 0; i < sizeof errands / sizeof *errands; i++) {
        // Errands look the pull request up by number, so the branch is not sent, nor input they do not take.
        a = board_action_arguments(known(errands[i]), "o/r", 9, "feat/x", "text");
        CHECK_INT(json_count(a), 3); CHECK_STR(json_str(json_get(a, "action")), errands[i]);
        CHECK(json_is_null(json_get(a, "branch"))); CHECK(json_is_null(json_get(a, "input")));
        json_free(a);
    }
    const BoardAction *feedback = known("custom-feedback");
    a = board_action_arguments(feedback, "o/r", 9, "feat/x", "\t Use 404 \r\n");
    CHECK_INT(json_count(a), 4); CHECK_STR(json_str(json_get(a, "action")), "custom-feedback"); CHECK_STR(json_str(json_get(a, "input")), "Use 404"); json_free(a);
    a = board_action_arguments(feedback, "o/r", 9, NULL, " \n\t ");
    CHECK_INT(json_count(a), 3); CHECK(json_is_null(json_get(a, "input"))); json_free(a);
    a = board_action_arguments(feedback, "o/r", 9, NULL, NULL);
    CHECK_INT(json_count(a), 3); json_free(a);
    a = board_action_arguments(feedback, "o/r", 9, NULL, "line one\nline two");
    CHECK_STR(json_str(json_get(a, "input")), "line one\nline two"); json_free(a);
}
static void test_errands_are_offered_for_the_state_a_pull_request_is_in(void) {
    const char *always = "run,review,custom-feedback,pr-body-summary,delete-self-comments";
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"mergeable\":\"mergeable\",\"checks\":\"success\"}", 0), always);
    // A draft is offered the same errands.
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"draft\":true}", 0), always);
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"mergeable\":\"conflicting\"}", 0), "run,review,solve-conflicts,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"labels\":[\"Has-Conflicts\"]}", 0), "run,review,solve-conflicts,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"checks\":\"error\"}", 0), "run,review,fix-checks,custom-feedback,pr-body-summary,delete-self-comments");
    // A run still going has nothing to fix; a count of failed checks from the Checks tab outweighs the summary.
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"checks\":\"pending\"}", 0), always);
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"checks\":\"success\"}", 2), "run,review,fix-checks,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1}", -1), always);
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"labels\":[\"FEEDBACK-GIVEN\"]}", 0), "run,review,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered(NULL, "{\"number\":1,\"mergeable\":\"conflicting\",\"checks\":\"failure\",\"labels\":[\"feedback-given\"]}", 0),
                    "run,review,solve-conflicts,fix-checks,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    // Without a pull request, everything but fixing checks nobody has seen fail.
    CHECK_OWNED_STR(offered(NULL, NULL, 0), "run,review,solve-conflicts,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered(NULL, NULL, 1), "run,review,solve-conflicts,fix-checks,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
}
static void test_a_known_catalog_restricts_errands_to_those_it_lists(void) {
    const char *conflicted = "{\"number\":1,\"mergeable\":\"conflicting\",\"checks\":\"failure\",\"labels\":[\"feedback-given\"]}";
    // An empty catalog, or one with nothing readable in it, is not known yet.
    CHECK_OWNED_STR(offered("[]", conflicted, 0), "run,review,solve-conflicts,fix-checks,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered("[{\"id\":\"pr-body-summary\"},{\"label\":\"x\"},\"y\"]", conflicted, 0),
                    "run,review,solve-conflicts,fix-checks,implement-feedback,custom-feedback,pr-body-summary,delete-self-comments");
    CHECK_OWNED_STR(offered("{\"id\":\"pr-body-summary\",\"label\":\"PR body\"}", "{\"number\":1}", 0), "run,review,custom-feedback,pr-body-summary,delete-self-comments");
    // Run and Code review have routes of their own and stay.
    CHECK_OWNED_STR(offered("[{\"id\":\"pr-body-summary\",\"label\":\"PR body\"}]", conflicted, 0), "run,review,pr-body-summary");
    // Listed is not enough: the pull request has to be in the state for it.
    CHECK_OWNED_STR(offered("[{\"id\":\"solve-conflicts\",\"label\":\"S\"},{\"id\":\"fix-checks\",\"label\":\"F\"},{\"id\":\"implement-feedback\",\"label\":\"I\"}]",
                            "{\"number\":1}", 0), "run,review");
    CHECK_OWNED_STR(offered("[{\"id\":\"implement-feedback\",\"label\":\"I\"},{\"id\":\"solve-conflicts\",\"label\":\"S\"}]", conflicted, 0),
                    "run,review,solve-conflicts,implement-feedback");
}
static void test_errands_the_app_does_not_know_follow_in_the_servers_order(void) {
    const char *catalog =
        "[{\"id\":\"zz-last\",\"label\":\"Z\"},{\"id\":\"run\",\"label\":\"Serve\"},{\"id\":\"qa\",\"label\":\"QA\"},{\"id\":\"test-sheet\",\"label\":\"Sheet\"},"
        "{\"id\":\"test-run\",\"label\":\"Run sheet\"},{\"id\":\"aa-first\",\"label\":\"A\",\"hint\":\"Does a\",\"input\":{\"label\":\"Why?\"}},"
        "{\"id\":\"zz-last\",\"label\":\"Z again\",\"hint\":\"later wins\"}]";
    Json *c = json_parsez(catalog);
    size_t n; BoardAction *a = board_actions_offered(c, NULL, 0, &n);
    // QA, the test sheet and its run stay gone even when the server lists them (#12).
    CHECK_OWNED_STR(action_ids(a, n), "run,review,zz-last,aa-first");
    if (n == 4) {
        // The app words its own errands; the server's label for one it knows is not used.
        CHECK_STR(a[0].label, "Run"); CHECK(!a[0].has_input);
        CHECK_STR(a[2].label, "Z again"); CHECK_STR(a[2].hint, "later wins"); CHECK(!a[2].has_input);
        CHECK_STR(a[3].label, "A"); CHECK_STR(a[3].hint, "Does a"); CHECK(a[3].has_input);
        CHECK_STR(a[3].input.label, "Why?"); CHECK_STR(a[3].input.placeholder, ""); CHECK(!a[3].input.required);
        CHECK_OWNED_STR(board_action_operation(&a[3]), "action");
        Json *args = board_action_arguments(&a[3], "o/r", 4, "b", " because ");
        CHECK_INT(json_count(args), 4); CHECK_STR(json_str(json_get(args, "action")), "aa-first"); CHECK_STR(json_str(json_get(args, "input")), "because");
        json_free(args);
        args = board_action_arguments(&a[2], "o/r", 4, "b", "nothing to take it");
        CHECK_INT(json_count(args), 3); CHECK_STR(json_str(json_get(args, "action")), "zz-last"); json_free(args);
    }
    board_actions_free(a, n); json_free(c);
}
static void test_the_server_words_the_question_an_errand_asks(void) {
    Json *c = json_parsez("[{\"id\":\"custom-feedback\",\"label\":\"Feedback\",\"input\":{\"label\":\"Tell it\",\"placeholder\":\"e.g. rename\",\"required\":false}},"
                          "{\"id\":\"pr-body-summary\",\"label\":\"PR body\",\"input\":{\"label\":\"Tone?\",\"required\":true}},"
                          "{\"id\":\"delete-self-comments\",\"label\":\"Delete\",\"input\":{\"placeholder\":\"no label\"}}]");
    size_t n; BoardAction *a = board_actions_offered(c, NULL, 0, &n);
    CHECK_OWNED_STR(action_ids(a, n), "run,review,custom-feedback,pr-body-summary,delete-self-comments");
    if (n == 5) {
        CHECK_STR(a[2].label, "Give feedback"); CHECK(a[2].has_input);
        CHECK_STR(a[2].input.label, "Tell it"); CHECK_STR(a[2].input.placeholder, "e.g. rename"); CHECK(!a[2].input.required);
        CHECK(a[3].has_input); CHECK_STR(a[3].input.label, "Tone?"); CHECK_STR(a[3].input.placeholder, ""); CHECK(a[3].input.required);
        // An input without a label is no input.
        CHECK(!a[4].has_input); CHECK(a[4].input.label == NULL);
    }
    board_actions_free(a, n); json_free(c);
    // Before the catalog is known, the app's own wording.
    a = board_actions_offered(NULL, NULL, 0, &n);
    for (size_t i = 0; i < n; i++) if (str_eq(a[i].id, "custom-feedback")) {
        CHECK(a[i].has_input && a[i].input.required); CHECK_STR(a[i].input.label, "Your feedback");
    }
    board_actions_free(a, n);
}
static void test_board_action_copies_are_independent(void) {
    BoardAction from = { xstrdup("x"), xstrdup("X"), NULL, true, { xstrdup("Q"), NULL, true } }, into;
    board_action_copy(&into, &from);
    board_action_free(&from); CHECK(from.id == NULL && !from.has_input);
    CHECK_STR(into.id, "x"); CHECK_STR(into.label, "X"); CHECK_STR(into.hint, "");
    CHECK(into.has_input && into.input.required); CHECK_STR(into.input.label, "Q"); CHECK(into.input.placeholder == NULL);
    board_action_free(&into);
    // Input parts are dropped when there is no input.
    BoardAction stray = { "y", "Y", "h", false, { "stray", "p", true } };
    board_action_copy(&into, &stray);
    CHECK_STR(into.hint, "h"); CHECK(!into.has_input && into.input.label == NULL && into.input.placeholder == NULL && !into.input.required);
    board_action_free(&into);
    board_action_free(NULL);
    board_actions_free(NULL, 0);
}

// MARK: - Merge warnings

static char *warnings(const char *mergeable, const char *state) {
    Json *m = mergeable ? json_parsez(mergeable) : NULL;
    size_t n = 99; char **w = merge_warnings(m, state, &n);
    Str s; str_init(&s);
    size_t i = 0;
    for (; w[i]; i++) str_appendf(&s, "%s%s", i ? "|" : "", w[i]);
    if (i != n) str_appendz(&s, " (count disagrees)");
    for (i = 0; w[i]; i++) free(w[i]);
    free(w); json_free(m);
    return s.data ? str_detach(&s) : xstrdup("");
}
static void test_merge_warnings_cover_every_mergeable_state(void) {
    const char *conflicts = "This branch has conflicts that must be resolved before it can merge.";
    const char *checking = "GitHub is still checking whether this branch can merge.";
    const char *blocked = "GitHub reports this pull request as blocked: a required review or check is missing.";
    const char *behind = "This branch is behind its base branch and may need updating before it can merge.";
    const char *quiet[] = { "clean", "unstable", "has_hooks", "dirty", "draft", "unknown", "closed", "merged", "BLOCKED", "Behind", "", NULL };
    for (size_t i = 0; i < sizeof quiet / sizeof *quiet; i++) CHECK_OWNED_STR(warnings("true", quiet[i]), "");
    CHECK_OWNED_STR(warnings("false", "dirty"), conflicts);
    CHECK_OWNED_STR(warnings("null", "unknown"), checking);
    // Nothing sent is still being checked; a string is not GitHub's answer either way.
    CHECK_OWNED_STR(warnings(NULL, NULL), checking);
    CHECK_OWNED_STR(warnings("\"false\"", "clean"), "");
    CHECK_OWNED_STR(warnings("0", "clean"), "");
    CHECK_OWNED_STR(warnings("true", "blocked"), blocked);
    CHECK_OWNED_STR(warnings("true", "behind"), behind);
    char *both = xstrfmt("%s|%s", conflicts, behind); CHECK_OWNED_STR(warnings("false", "behind"), both); free(both);
    both = xstrfmt("%s|%s", checking, blocked); CHECK_OWNED_STR(warnings("null", "blocked"), both); free(both);
    // Closed or merged, only a conflict still gets a word.
    CHECK_OWNED_STR(warnings("false", "closed"), conflicts);
    CHECK_OWNED_STR(warnings("false", "merged"), conflicts);
}

// MARK: - Reviews

static ReviewStatus status_of(const char *decision, const char *reviews) {
    Json *r = reviews ? json_parsez(reviews) : NULL;
    ReviewStatus s = review_status(decision, r);
    json_free(r);
    return s;
}
static void test_review_status_weighs_the_decision_before_the_reviewers(void) {
    CHECK(status_of(NULL, NULL) == REVIEW_NONE);
    CHECK(status_of(NULL, "[]") == REVIEW_NONE);
    CHECK(status_of("", "[]") == REVIEW_NONE);
    CHECK(status_of("APPROVED", NULL) == REVIEW_APPROVED);
    CHECK(status_of("Changes_Requested", NULL) == REVIEW_CHANGES_REQUESTED);
    CHECK(status_of("REVIEW_REQUIRED", NULL) == REVIEW_REQUESTED);
    // GitHub's own decision wins over what reviewers said.
    CHECK(status_of("approved", "[{\"state\":\"CHANGES_REQUESTED\"}]") == REVIEW_APPROVED);
    CHECK(status_of("changes_requested", "[{\"state\":\"APPROVED\"}]") == REVIEW_CHANGES_REQUESTED);
    // Without one, changes outweigh approval, approval feedback, and feedback a request.
    CHECK(status_of(NULL, "[{\"state\":\"approved\"},{\"state\":\"changes_requested\"},{\"state\":\"commented\"}]") == REVIEW_CHANGES_REQUESTED);
    CHECK(status_of(NULL, "[{\"state\":\"commented\"},{\"state\":\"APPROVED\"}]") == REVIEW_APPROVED);
    CHECK(status_of(NULL, "[{\"state\":\"requested\"},{\"state\":\"Commented\"}]") == REVIEW_FEEDBACK);
    CHECK(status_of("review_required", "[{\"state\":\"commented\"}]") == REVIEW_FEEDBACK);
    CHECK(status_of("review_required", "[{\"state\":\"approved\"}]") == REVIEW_APPROVED);
    CHECK(status_of(NULL, "[{\"state\":\"REQUESTED\"}]") == REVIEW_REQUESTED);
    CHECK(status_of("unknown", "[{\"state\":\"dismissed\"},{\"state\":\"pending\"},{},{\"state\":3},\"approved\"]") == REVIEW_NONE);
    CHECK(status_of(NULL, "{\"state\":\"approved\"}") == REVIEW_NONE);
}
static void test_review_status_reads_the_board_rows_reviewers(void) {
    Reviewer r[] = { { "ana", "requested" }, { "luis", "COMMENTED" }, { "bo", "" }, { "cy", NULL } };
    CHECK(review_status_of_reviewers(NULL, r, 4) == REVIEW_FEEDBACK);
    CHECK(review_status_of_reviewers(NULL, r, 1) == REVIEW_REQUESTED);
    CHECK(review_status_of_reviewers(NULL, r + 2, 2) == REVIEW_NONE);
    CHECK(review_status_of_reviewers("APPROVED", r, 4) == REVIEW_APPROVED);
    CHECK(review_status_of_reviewers(NULL, NULL, 0) == REVIEW_NONE);
    CHECK(review_status_of_reviewers("review_required", NULL, 0) == REVIEW_REQUESTED);
    PullSummary p;
    CHECK(pull_of("{\"number\":1,\"reviewers\":[{\"user\":\"a\",\"state\":\"approved\"},{\"user\":\"b\",\"state\":\"changes_requested\"}]}", &p));
    CHECK(review_status_of_reviewers(p.review_decision, p.reviewers, p.reviewer_count) == REVIEW_CHANGES_REQUESTED);
    pull_summary_free(&p);
    CHECK_STR(review_status_text(REVIEW_NONE), "");
    CHECK_STR(review_status_text(REVIEW_APPROVED), "Approved");
    CHECK_STR(review_status_text(REVIEW_CHANGES_REQUESTED), "Changes requested");
    CHECK_STR(review_status_text(REVIEW_FEEDBACK), "Feedback given");
    CHECK_STR(review_status_text(REVIEW_REQUESTED), "Review requested");
    CHECK_STR(review_status_text((ReviewStatus)42), "");
}

// MARK: - Stacks

static bool stack_of(const char *value, const char *stacks, StackPosition *out) {
    Json *v = json_parsez(value), *s = stacks ? json_parsez(stacks) : NULL;
    bool ok = stack_position_parse(v, s, out);
    json_free(v); json_free(s);
    return ok;
}
static void test_stack_positions_need_a_position_and_a_total(void) {
    StackPosition sp;
    CHECK(!stack_of("{\"total\":3}", NULL, &sp));
    CHECK(!stack_of("{\"position\":2}", NULL, &sp));
    CHECK(!stack_of("{\"position\":\"2\",\"total\":3}", NULL, &sp));
    CHECK(!stack_of("null", NULL, &sp));
    CHECK(!stack_position_parse(NULL, NULL, &sp));
    CHECK(stack_of("{\"position\":2,\"total\":3}", NULL, &sp));
    CHECK(sp.position == 2 && sp.total == 3 && !sp.partial && sp.base == NULL && sp.chain_count == 0);
    CHECK_OWNED_STR(stack_position_label(&sp, 0), "2/3");
    stack_position_free(&sp); CHECK(sp.chain == NULL && sp.total == 0);
    CHECK(stack_of("{\"position\":2,\"total\":3,\"partial\":true}", NULL, &sp)); CHECK(sp.partial);
    CHECK_OWNED_STR(stack_position_label(&sp, 0), "2/3+");
    stack_position_free(&sp);
    CHECK(stack_of("{\"position\":2,\"total\":3,\"partial\":\"yes\"}", NULL, &sp)); CHECK(!sp.partial); stack_position_free(&sp);
    stack_position_free(NULL);
}
static void test_stack_chains_are_found_by_id_number_or_string(void) {
    const char *stacks = "{\"5\":[{\"number\":1,\"title\":\"Base\",\"depth\":1,\"branch\":\"b1\"},{\"number\":2,\"depth\":2,\"headRef\":\"b2\"},"
                         "{\"title\":\"no number\"},{\"number\":3,\"depth\":3,\"branch\":\"\",\"headRef\":\"b3\",\"draft\":true},{\"number\":4,\"branch\":\"\"}],"
                         "\"abc\":[{\"number\":9}]}";
    StackPosition sp;
    CHECK(stack_of("{\"id\":5,\"position\":2,\"total\":4}", stacks, &sp));
    CHECK_INT(sp.chain_count, 4);
    if (sp.chain_count == 4) {
        CHECK_STR(sp.chain[0].title, "Base"); CHECK_STR(sp.chain[0].branch, "b1"); CHECK(!sp.chain[0].draft);
        CHECK_STR(sp.chain[1].title, "Pull request #2"); CHECK_STR(sp.chain[1].branch, "b2");
        CHECK_STR(sp.chain[2].branch, "b3"); CHECK(sp.chain[2].draft); CHECK_INT(sp.chain[2].depth, 3);
        // No depth reads as the bottom, and an empty branch as none.
        CHECK_INT(sp.chain[3].depth, 1); CHECK(sp.chain[3].branch == NULL);
    }
    stack_position_free(&sp);
    CHECK(stack_of("{\"id\":\"5\",\"position\":1,\"total\":4}", stacks, &sp)); CHECK_INT(sp.chain_count, 4); stack_position_free(&sp);
    CHECK(stack_of("{\"id\":\"abc\",\"position\":1,\"total\":1}", stacks, &sp)); CHECK_INT(sp.chain_count, 1); stack_position_free(&sp);
    // An id with no chain, or none at all, still places the pull request.
    CHECK(stack_of("{\"id\":6,\"position\":1,\"total\":2}", stacks, &sp)); CHECK(sp.chain_count == 0 && sp.total == 2); stack_position_free(&sp);
    CHECK(stack_of("{\"position\":1,\"total\":2}", stacks, &sp)); CHECK(sp.chain_count == 0); stack_position_free(&sp);
    CHECK(stack_of("{\"id\":\"abc\",\"position\":1,\"total\":2}", "{\"abc\":{\"number\":1}}", &sp)); CHECK(sp.chain_count == 0); stack_position_free(&sp);
}
static void test_stack_labels_show_each_items_own_depth(void) {
    StackPosition sp;
    CHECK(stack_of("{\"id\":1,\"position\":2,\"total\":3}", "{\"1\":[{\"number\":10,\"depth\":1},{\"number\":11,\"depth\":2},{\"number\":12,\"depth\":3}]}", &sp));
    CHECK_OWNED_STR(stack_position_label(&sp, 0), "2/3");
    CHECK_OWNED_STR(stack_position_label(&sp, 10), "1/3");
    CHECK_OWNED_STR(stack_position_label(&sp, 12), "3/3");
    // One not in the chain reads as the stack's own position.
    CHECK_OWNED_STR(stack_position_label(&sp, 99), "2/3");
    sp.partial = true;
    CHECK_OWNED_STR(stack_position_label(&sp, 11), "2/3+");
    CHECK_OWNED_STR(stack_position_label(&sp, 0), "2/3+");
    stack_position_free(&sp);
}
static PullSummary *stack_rows(const char *json, size_t *count) {
    Json *j = json_parsez(json);
    PullSummary *rows = pull_summaries_parse(j, count);
    json_free(j);
    return rows;
}
static void test_stack_branches_come_from_the_boards_rows(void) {
    StackPosition sp;
    CHECK(stack_of("{\"id\":1,\"position\":3,\"total\":3}",
                   "{\"1\":[{\"number\":12,\"depth\":3},{\"number\":10,\"depth\":1},{\"number\":11,\"depth\":2,\"branch\":\"own\"}]}", &sp));
    size_t n; PullSummary *rows = stack_rows("[{\"number\":10,\"branch\":\"b10\",\"baseBranch\":\"master\"},{\"number\":11,\"branch\":\"b11\",\"baseBranch\":\"b10\"},"
                                             "{\"number\":12,\"branch\":\"\",\"baseBranch\":\"b11\"},{\"number\":99,\"branch\":\"x\"}]", &n);
    stack_position_branches(&sp, rows, n);
    // A branch the chain names is kept, an empty one in a row is not lent, and the bottom by depth lends its base.
    CHECK(sp.chain[0].branch == NULL); CHECK_STR(sp.chain[1].branch, "b10"); CHECK_STR(sp.chain[2].branch, "own");
    CHECK_STR(sp.base, "master");
    pull_summaries_free(rows, n);
    // A later look replaces the base; a bottom without one keeps what was known.
    rows = stack_rows("[{\"number\":10,\"baseBranch\":\"main\"}]", &n);
    stack_position_branches(&sp, rows, n); CHECK_STR(sp.base, "main"); CHECK_STR(sp.chain[1].branch, "b10");
    pull_summaries_free(rows, n);
    rows = stack_rows("[{\"number\":10}]", &n);
    stack_position_branches(&sp, rows, n); CHECK_STR(sp.base, "main");
    pull_summaries_free(rows, n);
    stack_position_branches(&sp, NULL, 0); CHECK_STR(sp.base, "main");
    stack_position_free(&sp);
    // A partial chain that reaches the bottom still knows its base.
    CHECK(stack_of("{\"id\":1,\"position\":2,\"total\":3,\"partial\":true}", "{\"1\":[{\"number\":10,\"depth\":1},{\"number\":11,\"depth\":2}]}", &sp));
    rows = stack_rows("[{\"number\":10,\"branch\":\"b10\",\"baseBranch\":\"master\"}]", &n);
    stack_position_branches(&sp, rows, n); CHECK_STR(sp.base, "master");
    stack_position_free(&sp);
    // An empty chain learns nothing.
    CHECK(stack_of("{\"position\":1,\"total\":1}", NULL, &sp));
    stack_position_branches(&sp, rows, n); CHECK(sp.base == NULL);
    stack_position_free(&sp); pull_summaries_free(rows, n);
}
static void test_stacks_survive_a_save_and_restore(void) {
    StackPosition sp;
    CHECK(stack_of("{\"id\":1,\"position\":2,\"total\":3,\"partial\":true}",
                   "{\"1\":[{\"number\":10,\"title\":\"Bottom\",\"depth\":1,\"branch\":\"b10\"},{\"number\":11,\"title\":\"Mid\",\"depth\":2,\"draft\":true},"
                   "{\"number\":12,\"depth\":3,\"branch\":\"b12\"}]}", &sp));
    sp.base = xstrdup("master");
    Json *saved = stack_position_json(&sp);
    CHECK(json_bool_is(json_get(saved, "partial"), true)); CHECK_STR(json_str(json_get(saved, "base")), "master");
    const Json *chain = json_get(saved, "chain");
    CHECK_INT(json_count(chain), 3);
    // Only a draft says so, and an item without a branch leaves it out.
    CHECK(json_is_null(json_get(json_at(chain, 0), "draft"))); CHECK(json_bool_is(json_get(json_at(chain, 1), "draft"), true));
    CHECK(json_is_null(json_get(json_at(chain, 1), "branch"))); CHECK_STR(json_str(json_get(json_at(chain, 2), "title")), "Pull request #12");
    StackPosition back; CHECK(stack_position_restore(saved, &back));
    CHECK(back.position == 2 && back.total == 3 && back.partial); CHECK_STR(back.base, "master");
    CHECK_INT(back.chain_count, 3);
    for (size_t i = 0; i < back.chain_count && i < sp.chain_count; i++) {
        CHECK_INT(back.chain[i].number, sp.chain[i].number); CHECK_STR(back.chain[i].title, sp.chain[i].title);
        CHECK_STR(back.chain[i].branch, sp.chain[i].branch); CHECK_INT(back.chain[i].depth, sp.chain[i].depth); CHECK(back.chain[i].draft == sp.chain[i].draft);
    }
    CHECK_OWNED_STR(stack_position_label(&back, 11), "2/3+");
    Json *again = stack_position_json(&back); CHECK(json_equal(saved, again)); json_free(again);
    stack_position_free(&back); json_free(saved); stack_position_free(&sp);
    // Without a base or chain, neither is saved nor restored.
    CHECK(stack_of("{\"position\":1,\"total\":1}", NULL, &sp));
    saved = stack_position_json(&sp);
    CHECK(json_is_null(json_get(saved, "base"))); CHECK_INT(json_count(json_get(saved, "chain")), 0); CHECK(json_bool_is(json_get(saved, "partial"), false));
    CHECK(stack_position_restore(saved, &back)); CHECK(back.base == NULL && back.chain_count == 0 && !back.partial);
    stack_position_free(&back); json_free(saved); stack_position_free(&sp);
    Json *bad = json_parsez("{\"position\":1,\"base\":\"\"}"); CHECK(!stack_position_restore(bad, &back)); json_free(bad);
    bad = json_parsez("{\"position\":1,\"total\":1,\"base\":\"\",\"chain\":[{\"title\":\"x\"}]}");
    CHECK(stack_position_restore(bad, &back)); CHECK(back.base == NULL && back.chain_count == 0); stack_position_free(&back); json_free(bad);
    CHECK(!stack_position_restore(NULL, &back));
}
static void test_stack_copies_are_independent(void) {
    StackPosition sp, copy;
    CHECK(stack_of("{\"id\":1,\"position\":2,\"total\":2,\"partial\":true}", "{\"1\":[{\"number\":10,\"title\":\"A\",\"branch\":\"b\"},{\"number\":11,\"depth\":2,\"draft\":true}]}", &sp));
    sp.base = xstrdup("main");
    stack_position_copy(&copy, &sp);
    CHECK(copy.chain != sp.chain && copy.chain_count == 2);
    stack_position_free(&sp);
    CHECK(copy.position == 2 && copy.total == 2 && copy.partial); CHECK_STR(copy.base, "main");
    CHECK_STR(copy.chain[0].title, "A"); CHECK_STR(copy.chain[0].branch, "b"); CHECK(copy.chain[1].branch == NULL);
    CHECK(copy.chain[1].draft && copy.chain[1].depth == 2 && copy.chain[1].number == 11);
    stack_position_free(&copy);
    CHECK(stack_of("{\"position\":1,\"total\":1}", NULL, &sp));
    stack_position_copy(&copy, &sp); CHECK(copy.base == NULL && copy.chain_count == 0);
    stack_position_free(&copy); stack_position_free(&sp);
}
static char *top_first(const char *chain) {
    StackPosition sp; char *stacks = xstrfmt("{\"1\":%s}", chain);
    stack_of("{\"id\":1,\"position\":1,\"total\":9}", stacks, &sp); free(stacks);
    size_t *order = stack_position_top_first(&sp);
    Str s; str_init(&s);
    for (size_t i = 0; i < sp.chain_count; i++) str_appendf(&s, "%s%d", i ? "," : "", sp.chain[order[i]].number);
    free(order); stack_position_free(&sp);
    return s.data ? str_detach(&s) : xstrdup("");
}
static void test_stack_overviews_list_the_top_first(void) {
    CHECK_OWNED_STR(top_first("[{\"number\":1,\"depth\":1},{\"number\":2,\"depth\":2},{\"number\":3,\"depth\":3}]"), "3,2,1");
    CHECK_OWNED_STR(top_first("[{\"number\":3,\"depth\":3},{\"number\":1,\"depth\":1},{\"number\":2,\"depth\":2}]"), "3,2,1");
    // Siblings at one depth keep the server's order.
    CHECK_OWNED_STR(top_first("[{\"number\":1,\"depth\":1},{\"number\":4,\"depth\":2},{\"number\":2,\"depth\":2},{\"number\":5,\"depth\":3},{\"number\":3,\"depth\":2}]"),
                    "5,4,2,3,1");
    CHECK_OWNED_STR(top_first("[{\"number\":7}]"), "7");
    CHECK_OWNED_STR(top_first("[]"), "");
}

// MARK: - Links out

static void test_only_https_urls_with_a_host_and_no_credentials_are_opened(void) {
    const char *safe[] = {
        "https://github.com/o/r/pull/1", "https://github.com", "https://github.com/", "https://github.com:443/x", "https://a.b?q=1",
        "https://a.b#frag", "https://github.com/user@example/x", "https://github.com/?u=a@b", "https://x#@y",
    };
    for (size_t i = 0; i < sizeof safe / sizeof *safe; i++) CHECK(safe_web_url(safe[i]));
    const char *unsafe[] = {
        NULL, "", "http://github.com", "HTTPS://github.com", " https://github.com", "https:github.com", "https:/github.com",
        "https://", "https:///path", "https://?q", "https://#x", "https://user@github.com", "https://user:pass@github.com/x",
        "https://github.com@evil.example", "https://@github.com", "javascript:alert(1)", "javascript://https://x", "file:///C:/x",
        "ftp://github.com", "//github.com", "github.com",
    };
    for (size_t i = 0; i < sizeof unsafe / sizeof *unsafe; i++) {
        bool ok = safe_web_url(unsafe[i]);
        CHECK(!ok);
        if (ok) printf("    accepted %s\n", unsafe[i] ? unsafe[i] : "(null)");
    }
}

// MARK: - ▶ Run

static void test_run_profiles_are_read_for_the_project_default_first(void) {
    Json *j = json_parsez("{\"projects\":[{\"repo\":\"o/other\",\"runProfiles\":[\"x\"]},"
                         "{\"repo\":\"o/r\",\"runProfiles\":[\"veterinary_central\",\"\",\"demo\",7]}]}");
    size_t n = 99;
    char **p = run_profiles_parse(j, "o/r", &n);
    CHECK_INT(n, 2); CHECK_STR(p[0], "veterinary_central"); CHECK_STR(p[1], "demo"); CHECK(p[2] == NULL);
    str_array_free(p, n);
    // The bare array reads the same.
    p = run_profiles_parse(json_get(j, "projects"), "o/other", &n);
    CHECK_INT(n, 1); CHECK_STR(p[0], "x"); str_array_free(p, n);
    json_free(j);
}
static void test_run_profiles_are_empty_when_none_or_unlisted(void) {
    Json *j = json_parsez("{\"projects\":[{\"repo\":\"o/r\"},{\"repo\":\"o/s\",\"runProfiles\":[]}]}");
    size_t n = 99;
    char **p = run_profiles_parse(j, "o/r", &n); CHECK_INT(n, 0); CHECK(p && p[0] == NULL); str_array_free(p, n);
    p = run_profiles_parse(j, "o/s", &n); CHECK_INT(n, 0); str_array_free(p, n);
    p = run_profiles_parse(j, "o/missing", &n); CHECK_INT(n, 0); CHECK(p && p[0] == NULL); str_array_free(p, n);
    p = run_profiles_parse(NULL, "o/r", &n); CHECK_INT(n, 0); str_array_free(p, n);
    json_free(j);
}
static void test_the_run_being_prepared_is_the_newest_run_session_on_the_pull_request(void) {
    Json *j = json_parsez("{\"sessions\":["
        "{\"id\":\"old\",\"title\":\"Run: #7 Fix\",\"startedOnPr\":7,\"createdAt\":\"2026-10-01T10:00:00Z\"},"
        "{\"id\":\"chat\",\"title\":\"Fix the bug\",\"startedOnPr\":7,\"createdAt\":\"2026-10-01T12:00:00Z\"},"
        "{\"id\":\"other\",\"title\":\"Run: #8\",\"startedOnPr\":8,\"createdAt\":\"2026-10-01T12:00:00Z\"},"
        "{\"id\":\"new\",\"title\":\"Run: #7 Fix\",\"prStatus\":{\"number\":7},\"createdAt\":\"2026-10-01T11:00:00Z\"}]}");
    CHECK_OWNED_STR(run_session_preparing(j, 7), "new");
    CHECK_OWNED_STR(run_session_preparing(j, 8), "other");
    CHECK(run_session_preparing(j, 9) == NULL);
    CHECK(run_session_preparing(NULL, 7) == NULL);
    json_free(j);
}
static void test_a_run_already_serving_is_found_by_its_serve_link(void) {
    Json *j = json_parsez("{\"sessions\":["
        "{\"id\":\"idle\",\"startedOnPr\":7,\"serveLinks\":null},"
        "{\"id\":\"local\",\"startedOnPr\":7,\"serveLinks\":[{\"url\":\"http://127.0.0.1:8123\"}]},"
        "{\"id\":\"elsewhere\",\"startedOnPr\":8,\"serveLinks\":[{\"url\":\"https://8124.preview.example.com\"}]},"
        "{\"id\":\"live\",\"startedOnPr\":7,\"serveLinks\":[{\"tenant\":\"a\",\"url\":\"https://a-8125.preview.example.com\"},{\"url\":\"https://b\"}]}]}");
    char *id = NULL, *url = NULL;
    // Only an https link is opened in the tab.
    CHECK(run_session_serving(j, 7, &id, &url));
    CHECK_STR(id, "live"); CHECK_STR(url, "https://a-8125.preview.example.com"); free(id); free(url);
    id = url = NULL;
    CHECK(run_session_serving(json_get(j, "sessions"), 8, &id, &url)); CHECK_STR(id, "elsewhere"); free(id); free(url);
    id = url = NULL;
    CHECK(!run_session_serving(j, 9, &id, &url)); CHECK(id == NULL && url == NULL);
    json_free(j);
}
static void test_the_run_log_reads_log_lines_past_its_cursor(void) {
    RunLog log; memset(&log, 0, sizeof log);
    Json *events = json_parsez("["
        "{\"seq\":1,\"kind\":\"user\",\"text\":\"hello\"},"
        "{\"seq\":2,\"kind\":\"info\",\"text\":\"Preparing pull request #7\"},"
        "{\"seq\":3,\"kind\":\"cmd\",\"text\":\"composer install\"},"
        "{\"seq\":4,\"kind\":\"setup\",\"text\":\"Installing\\r\\n\\nDone\"},"
        "{\"seq\":5,\"kind\":\"stderr\",\"text\":\"npm WARN deprecated\"},"
        "{\"seq\":6,\"kind\":\"status\",\"status\":\"idle\"},"
        "{\"seq\":7,\"kind\":\"text\",\"text\":\"the agent speaking\"},"
        "{\"seq\":8,\"kind\":\"git\",\"text\":\"Cloning\"},"
        "{\"seq\":9,\"kind\":\"tool\",\"text\":\"Bash\"}]");
    CHECK(run_log_add_events(&log, events));
    CHECK_INT(log.count, 7); CHECK_INT((int)log.cursor, 9);
    CHECK_STR(log.lines[0], "Preparing pull request #7");
    CHECK_STR(log.lines[1], "$ composer install");
    CHECK_STR(log.lines[2], "Installing"); CHECK_STR(log.lines[3], "Done");
    CHECK_STR(log.lines[4], "npm WARN deprecated"); CHECK(log.errors[4]); CHECK(!log.errors[3]);
    CHECK_STR(log.lines[5], "\xE2\x80\xA2 idle");
    CHECK_STR(log.lines[6], "Cloning");
    // Read again from the same answer: nothing is added twice.
    CHECK(!run_log_add_events(&log, events)); CHECK_INT(log.count, 7);
    json_free(events);
    events = json_parsez("[{\"seq\":9,\"kind\":\"info\",\"text\":\"again\"},{\"seq\":10,\"kind\":\"info\",\"text\":\"Serving on 8123\"}]");
    CHECK(run_log_add_events(&log, events)); CHECK_INT(log.count, 8); CHECK_STR(log.lines[7], "Serving on 8123");
    json_free(events);
    run_log_clear(&log);
    CHECK_INT(log.count, 0); CHECK(log.lines == NULL); CHECK_INT((int)log.cursor, 0);
}
static void test_the_run_log_keeps_its_latest_lines(void) {
    RunLog log; memset(&log, 0, sizeof log);
    CHECK_INT(run_log_add(&log, "", false), 0);
    CHECK_INT(run_log_add(&log, "one\ntwo\n", false), 2);
    for (int i = 0; i < RUN_LOG_CAP; i++) { char line[16]; snprintf(line, sizeof line, "line %d", i); run_log_add(&log, line, i % 2 == 0); }
    CHECK_INT(log.count, RUN_LOG_CAP);
    CHECK_STR(log.lines[0], "line 0"); CHECK(log.errors[0]);
    CHECK_STR(log.lines[RUN_LOG_CAP - 1], "line 399"); CHECK(!log.errors[RUN_LOG_CAP - 1]);
    // A full log still reports what it added.
    Json *events = json_parsez("[{\"seq\":1,\"kind\":\"info\",\"text\":\"newest\"}]");
    CHECK(run_log_add_events(&log, events)); CHECK_INT(log.count, RUN_LOG_CAP);
    CHECK_STR(log.lines[0], "line 1"); CHECK_STR(log.lines[RUN_LOG_CAP - 1], "newest");
    json_free(events);
    run_log_clear(&log);
}

void board_tests(void) {
    test_run("labels read from an object or a bare name", test_labels_read_from_an_object_or_a_bare_name);
    test_run("label colours read as six hex digits", test_label_colours_read_as_six_hex_digits);
    test_run("board links need a number and default their title", test_board_links_need_a_number_and_default_their_title);
    test_run("board links name their repository only when foreign", test_board_links_name_their_repository_only_when_foreign);
    test_run("board links are not planned only when closed as such", test_board_links_are_not_planned_only_when_closed_as_such);
    test_run("board link copies are independent", test_board_link_copies_are_independent);
    test_run("pull summaries read every field", test_pull_summaries_read_every_field);
    test_run("pull summaries default what the server left out", test_pull_summaries_default_what_the_server_left_out);
    test_run("pull summaries without a positive number are rejected", test_pull_summaries_without_a_positive_number_are_rejected);
    test_run("pull summary lists keep only valid rows", test_pull_summary_lists_keep_only_valid_rows);
    test_run("pull summary copies are independent", test_pull_summary_copies_are_independent);
    test_run("pull predicates read mergeable, checks and labels", test_pull_predicates_read_mergeable_checks_and_labels);
    test_run("issue summaries read every field", test_issue_summaries_read_every_field);
    test_run("issue summaries default what the server left out", test_issue_summaries_default_what_the_server_left_out);
    test_run("issues nest depth first under epics on the list", test_issues_nest_depth_first_under_epics_on_the_list);
    test_run("issues in a cycle are each drawn once", test_issues_in_a_cycle_are_each_drawn_once);
    test_run("issue prompts name the issue, its epic and how to close it", test_issue_prompts_name_the_issue_its_epic_and_how_to_close_it);
    test_run("board rows carry what the pickers filter on", test_board_rows_carry_what_the_pickers_filter_on);
    test_run("board filters keep their picks folded", test_board_filters_keep_their_picks_folded);
    test_run("board filter copies compare equal and are independent", test_board_filter_copies_compare_equal_and_are_independent);
    test_run("board filters pass rows folding case", test_board_filters_pass_rows_folding_case);
    test_run("board filter options are counted against the other pickers", test_board_filter_options_are_counted_against_the_other_pickers);
    test_run("a pick the others empty still lists itself", test_a_pick_the_others_empty_still_lists_itself);
    test_run("issue rows offer no reviewers", test_issue_rows_offer_no_reviewers);
    test_run("the board opens on the author only while they have rows", test_the_board_opens_on_the_author_only_while_they_have_rows);
    test_run("known errands are listed in the dashboard's order", test_known_errands_are_listed_in_the_dashboards_order);
    test_run("errands start through their own operation", test_errands_start_through_their_own_operation);
    test_run("errand arguments take the shape each route wants", test_errand_arguments_take_the_shape_each_route_wants);
    test_run("errands are offered for the state a pull request is in", test_errands_are_offered_for_the_state_a_pull_request_is_in);
    test_run("a known catalog restricts errands to those it lists", test_a_known_catalog_restricts_errands_to_those_it_lists);
    test_run("errands the app does not know follow in the server's order", test_errands_the_app_does_not_know_follow_in_the_servers_order);
    test_run("the server words the question an errand asks", test_the_server_words_the_question_an_errand_asks);
    test_run("board action copies are independent", test_board_action_copies_are_independent);
    test_run("merge warnings cover every mergeable state", test_merge_warnings_cover_every_mergeable_state);
    test_run("review status weighs the decision before the reviewers", test_review_status_weighs_the_decision_before_the_reviewers);
    test_run("review status reads the board row's reviewers", test_review_status_reads_the_board_rows_reviewers);
    test_run("stack positions need a position and a total", test_stack_positions_need_a_position_and_a_total);
    test_run("stack chains are found by id, number or string", test_stack_chains_are_found_by_id_number_or_string);
    test_run("stack labels show each item's own depth", test_stack_labels_show_each_items_own_depth);
    test_run("stack branches come from the board's rows", test_stack_branches_come_from_the_boards_rows);
    test_run("stacks survive a save and restore", test_stacks_survive_a_save_and_restore);
    test_run("stack copies are independent", test_stack_copies_are_independent);
    test_run("stack overviews list the top first", test_stack_overviews_list_the_top_first);
    test_run("only https urls with a host and no credentials are opened", test_only_https_urls_with_a_host_and_no_credentials_are_opened);
    test_run("run profiles are read for the project, default first", test_run_profiles_are_read_for_the_project_default_first);
    test_run("run profiles are empty when none or unlisted", test_run_profiles_are_empty_when_none_or_unlisted);
    test_run("the run being prepared is the newest run session on the pull request", test_the_run_being_prepared_is_the_newest_run_session_on_the_pull_request);
    test_run("a run already serving is found by its serve link", test_a_run_already_serving_is_found_by_its_serve_link);
    test_run("the run log reads log lines past its cursor", test_the_run_log_reads_log_lines_past_its_cursor);
    test_run("the run log keeps its latest lines", test_the_run_log_keeps_its_latest_lines);
}
