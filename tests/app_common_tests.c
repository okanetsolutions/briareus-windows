// common.c: the labels, glyphs and badges the screens share, the poller's backoff bookkeeping, and the shared rows laid
// out headless (DirectWrite measures without a window).
#include "board.h"
#include "doc.h"
#include "json.h"
#include "models.h"
#include "screens.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>

static void ensure_theme(void) { if (!theme.canvas) theme_init(); }

static Session session_from(const char *json) {
    Json *j = json_parsez(json);
    Session s; memset(&s, 0, sizeof s);
    CHECK(j && session_parse(j, &s));
    json_free(j);
    return s;
}

/// The first item whose text is `text`, or -1.
static int item_with_text(Doc *doc, const char *text) {
    for (size_t i = 0; i < doc->count; i++) if (str_eq(doc->items[i].text, text)) return (int)i;
    return -1;
}

// MARK: - Labels

static void test_severity_labels_and_colours(void) {
    ensure_theme();
    COLORREF c = 0;
    CHECK_STR(finding_severity_label("critical", &c), "CRIT"); CHECK_INT(c, theme.danger);
    CHECK_STR(finding_severity_label("CRITICAL", &c), "CRIT"); CHECK_INT(c, theme.danger);
    CHECK_STR(finding_severity_label("High", &c), "HIGH"); CHECK_INT(c, theme.danger);
    CHECK_STR(finding_severity_label("low", &c), "LOW"); CHECK_INT(c, theme.muted);
    CHECK_STR(finding_severity_label("medium", &c), "MED"); CHECK_INT(c, theme.warn);
}

static void test_unknown_severity_reads_as_medium(void) {
    ensure_theme();
    COLORREF c = 0;
    CHECK_STR(finding_severity_label("", &c), "MED"); CHECK_INT(c, theme.warn);
    CHECK_STR(finding_severity_label(NULL, &c), "MED"); CHECK_INT(c, theme.warn);
    CHECK_STR(finding_severity_label("blocker", &c), "MED"); CHECK_INT(c, theme.warn);
}

static void test_severity_colours_are_the_ones_the_screens_used_before_sharing(void) {
    // #46 moved three copies here; the findings screen's copy named secondary and warning, the aliases of muted and warn.
    ensure_theme();
    COLORREF c = 0;
    finding_severity_label("low", &c); CHECK_INT(c, theme.secondary);
    finding_severity_label("medium", &c); CHECK_INT(c, theme.warning);
}

static void test_decisions_are_indexed_by_their_api_spelling(void) {
    CHECK_INT(FINDING_DECISION_COUNT, 3);
    CHECK_INT(finding_decision_index("fix"), 0);
    CHECK_INT(finding_decision_index("optional"), 1);
    CHECK_INT(finding_decision_index("dismissed"), 2);
    CHECK_STR(finding_decision_titles[0], "Fix");
    CHECK_STR(finding_decision_titles[1], "Optional");
    CHECK_STR(finding_decision_titles[2], "Dismiss");
    for (int k = 0; k < FINDING_DECISION_COUNT; k++) CHECK_INT(finding_decision_index(finding_decision_ids[k]), k);
}

static void test_unknown_decisions_have_no_index(void) {
    CHECK_INT(finding_decision_index("Fix"), -1);
    CHECK_INT(finding_decision_index("dismiss"), -1);
    CHECK_INT(finding_decision_index("undecided"), -1);
    CHECK_INT(finding_decision_index(""), -1);
    CHECK_INT(finding_decision_index(NULL), -1);
}

static void test_session_subtitle_is_status_and_model(void) {
    Session s = session_from("{\"id\":\"s1\",\"status\":\"waiting\",\"model\":\"opus\"}");
    CHECK_OWNED_STR(session_subtitle(&s), "Waiting \xC2\xB7 opus");
    session_free(&s);
    s = session_from("{\"id\":\"s2\",\"status\":\"idle\"}");
    CHECK_OWNED_STR(session_subtitle(&s), "Idle");
    session_free(&s);
    s = session_from("{\"id\":\"s3\",\"status\":\"running\",\"model\":\"\"}");
    CHECK_OWNED_STR(session_subtitle(&s), "Running");
    session_free(&s);
}

static void test_set_string_replaces_and_clears(void) {
    char *slot = NULL;
    set_string(&slot, "one");
    CHECK_STR(slot, "one");
    set_string(&slot, "two");
    CHECK_STR(slot, "two");
    set_string(&slot, NULL);
    CHECK(slot == NULL);
    set_string(&slot, "");
    CHECK_STR(slot, "");
    free(slot);
}

static void test_people_lists_logins_up_to_a_limit(void) {
    char *logins[] = { "ana", "bo", "cy", "di" };
    CHECK_OWNED_STR(people(logins, 1, 2), "@ana");
    CHECK_OWNED_STR(people(logins, 2, 2), "@ana, @bo");
    CHECK_OWNED_STR(people(logins, 3, 2), "@ana, @bo +1");
    CHECK_OWNED_STR(people(logins, 4, 2), "@ana, @bo +2");
    CHECK_OWNED_STR(people(logins, 4, 10), "@ana, @bo, @cy, @di");
    CHECK_OWNED_STR(people(logins, 0, 2), "");
}

// MARK: - Colours and glyphs

static void test_label_colour_is_githubs_hex_or_secondary(void) {
    ensure_theme();
    PullLabel bug = { "bug", "d73a4a" }, upper = { "x", "00FF7f" };
    CHECK_INT(label_color(&bug), RGB(0xd7, 0x3a, 0x4a));
    CHECK_INT(label_color(&upper), RGB(0x00, 0xff, 0x7f));
    PullLabel none = { "n", NULL }, hash = { "h", "#d73a4a" }, short_hex = { "s", "fff" }, junk = { "j", "zzzzzz" };
    CHECK_INT(label_color(&none), theme.secondary);
    CHECK_INT(label_color(&hash), theme.secondary);
    CHECK_INT(label_color(&short_hex), theme.secondary);
    CHECK_INT(label_color(&junk), theme.secondary);
}

static void test_review_glyphs_and_colours(void) {
    ensure_theme();
    COLORREF c = 0;
    CHECK_INT(review_glyph(REVIEW_APPROVED, &c), 0xE73E); CHECK_INT(c, theme.success);
    CHECK_INT(review_glyph(REVIEW_CHANGES_REQUESTED, &c), 0xE711); CHECK_INT(c, theme.danger);
    CHECK_INT(review_glyph(REVIEW_FEEDBACK, &c), 0xE8BD); CHECK_INT(c, theme.warning);
    CHECK_INT(review_glyph(REVIEW_REQUESTED, &c), 0xE823); CHECK_INT(c, theme.secondary);
    CHECK_INT(review_glyph(REVIEW_NONE, &c), 0); CHECK_INT(c, theme.secondary);
}

static void test_check_glyphs_group_conclusions(void) {
    ensure_theme();
    COLORREF c = 0;
    const char *green[] = { "success", "passed", "neutral", "skipped", "SUCCESS" };
    for (size_t i = 0; i < 5; i++) { c = 0; CHECK_INT(check_glyph(green[i], &c), 0xE930); CHECK_INT(c, theme.success); }
    const char *red[] = { "failure", "failed", "timed_out", "action_required", "error", "Failure" };
    for (size_t i = 0; i < 6; i++) { c = 0; CHECK_INT(check_glyph(red[i], &c), 0xEA39); CHECK_INT(c, theme.danger); }
    const char *grey[] = { "cancelled", "stale" };
    for (size_t i = 0; i < 2; i++) { c = 0; CHECK_INT(check_glyph(grey[i], &c), 0xE738); CHECK_INT(c, theme.secondary); }
}

static void test_pending_or_unknown_checks_are_amber(void) {
    ensure_theme();
    COLORREF c = 0;
    CHECK_INT(check_glyph("pending", &c), 0xE823); CHECK_INT(c, theme.warning);
    CHECK_INT(check_glyph("in_progress", &c), 0xE823); CHECK_INT(c, theme.warning);
    CHECK_INT(check_glyph(NULL, &c), 0xE823); CHECK_INT(c, theme.warning);
}

static void test_tool_glyphs_follow_the_tool_name(void) {
    CHECK_INT(tool_glyph("tool", "Bash", false), 0xE756);
    CHECK_INT(tool_glyph("tool", "shell_command", false), 0xE756);
    CHECK_INT(tool_glyph("tool", "Read", false), 0xE8A5);
    CHECK_INT(tool_glyph("tool", "view_image", false), 0xE8A5);
    CHECK_INT(tool_glyph("tool", "Edit", false), 0xE70F);
    CHECK_INT(tool_glyph("tool", "Write", false), 0xE70F);
    CHECK_INT(tool_glyph("tool", "apply_patch", false), 0xE70F);
    CHECK_INT(tool_glyph("tool", "Grep", false), 0xE721);
    CHECK_INT(tool_glyph("tool", "Glob", false), 0xE721);
    CHECK_INT(tool_glyph("tool", "WebSearch", false), 0xE721);
    CHECK_INT(tool_glyph("tool", "WebFetch", false), 0xE774);
    CHECK_INT(tool_glyph("tool", "update_plan", false), 0xE9D5);
    CHECK_INT(tool_glyph("tool", "Task", false), 0xE716);
    CHECK_INT(tool_glyph("tool", "Agent", false), 0xE716);
    CHECK_INT(tool_glyph("tool", "mcp__linear__list", false), 0xE90F);
    CHECK_INT(tool_glyph("tool", NULL, false), 0xE90F);
}

static void test_tool_glyph_kind_and_errors_come_first(void) {
    CHECK_INT(tool_glyph("tool_error", "Read", true), 0xE7BA);
    CHECK_INT(tool_glyph("cmd", "Read", true), 0xE7BA);
    CHECK_INT(tool_glyph("cmd", "Read", false), 0xE756);
    CHECK_INT(tool_glyph("git", "Read", false), 0xE8AB);
    CHECK_INT(tool_glyph("git", NULL, false), 0xE8AB);
}

// MARK: - Pull request badges

static void test_pull_badges_in_order(void) {
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    p.mergeable = "conflicting"; p.checks = "success"; p.review_decision = "APPROVED"; p.draft = true;
    StackPosition stack; memset(&stack, 0, sizeof stack);
    stack.position = 2; stack.total = 3; stack.partial = true;
    BadgeSpec b[8]; char stack_text[32];
    size_t n = pull_badges(&p, &stack, b, 8, stack_text, sizeof stack_text);
    CHECK_INT(n, 5);
    CHECK_STR(b[0].text, "Conflicts"); CHECK_INT(b[0].glyph, 0xE7BA); CHECK_INT(b[0].color, theme.danger);
    CHECK_STR(b[1].text, "Checks"); CHECK_INT(b[1].glyph, 0xE73E); CHECK_INT(b[1].color, theme.success);
    CHECK_STR(b[2].text, "Stack 2/3+"); CHECK_INT(b[2].glyph, 0xE81E); CHECK_INT(b[2].color, theme.accent);
    CHECK(b[2].text == stack_text);
    CHECK_STR(b[3].text, "Approved"); CHECK_INT(b[3].glyph, 0xE73E); CHECK_INT(b[3].color, theme.success);
    CHECK_STR(b[4].text, "Draft"); CHECK_INT(b[4].glyph, 0xE70F); CHECK_INT(b[4].color, theme.secondary);
    for (size_t i = 0; i < n; i++) CHECK(!b[i].chip);
}

static void test_pull_badges_stop_at_the_capacity(void) {
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    p.mergeable = "conflicting"; p.checks = "failure"; p.draft = true;
    BadgeSpec b[2]; char stack_text[32];
    CHECK_INT(pull_badges(&p, NULL, b, 2, stack_text, sizeof stack_text), 2);
    CHECK_STR(b[0].text, "Conflicts");
    CHECK_STR(b[1].text, "Checks");
    CHECK_INT(pull_badges(&p, NULL, b, 0, stack_text, sizeof stack_text), 0);
}

static void test_pull_badges_for_check_states(void) {
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    BadgeSpec b[4]; char stack_text[32];
    CHECK_INT(pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text), 0);
    p.checks = "failure";
    CHECK_INT(pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text), 1);
    CHECK_INT(b[0].glyph, 0xE711); CHECK_INT(b[0].color, theme.danger);
    p.checks = "error";
    pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text);
    CHECK_INT(b[0].glyph, 0xE711); CHECK_INT(b[0].color, theme.danger);
    p.checks = "pending";
    pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text);
    CHECK_STR(b[0].text, "Checks"); CHECK_INT(b[0].glyph, 0xE823); CHECK_INT(b[0].color, theme.warning);
}

static void test_conflicts_badge_needs_githubs_answer_not_the_label(void) {
    ensure_theme();
    PullLabel label = { "has-conflicts", "ff0000" };
    PullSummary p; memset(&p, 0, sizeof p);
    p.labels = &label; p.label_count = 1; p.mergeable = "unknown";
    BadgeSpec b[4]; char stack_text[32];
    CHECK_INT(pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text), 0);
}

static void test_pull_badges_review_from_the_reviewers(void) {
    ensure_theme();
    Reviewer commented = { "ana", "COMMENTED" }, requested = { "bo", "REQUESTED" };
    PullSummary p; memset(&p, 0, sizeof p);
    BadgeSpec b[4]; char stack_text[32];
    p.reviewers = &commented; p.reviewer_count = 1;
    CHECK_INT(pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text), 1);
    CHECK_STR(b[0].text, "Feedback given"); CHECK_INT(b[0].glyph, 0xE8BD); CHECK_INT(b[0].color, theme.warning);
    p.reviewers = &requested;
    pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text);
    CHECK_STR(b[0].text, "Review requested"); CHECK_INT(b[0].color, theme.secondary);
    p.reviewers = NULL; p.reviewer_count = 0; p.review_decision = "CHANGES_REQUESTED";
    pull_badges(&p, NULL, b, 4, stack_text, sizeof stack_text);
    CHECK_STR(b[0].text, "Changes requested"); CHECK_INT(b[0].glyph, 0xE711); CHECK_INT(b[0].color, theme.danger);
}

static void test_stack_badge_text_is_cut_to_its_buffer(void) {
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    StackPosition stack; memset(&stack, 0, sizeof stack);
    stack.position = 12; stack.total = 34;
    BadgeSpec b[2]; char stack_text[8];
    CHECK_INT(pull_badges(&p, &stack, b, 2, stack_text, sizeof stack_text), 1);
    CHECK_STR(b[0].text, "Stack 1");
}

// MARK: - Poller

static void test_poller_counts_failures_and_keeps_retry_after(void) {
    Poller p; memset(&p, 0, sizeof p);
    p.base_ms = 5000;
    poller_finished(&p, true, 7);
    CHECK_INT(p.failures, 1);
    CHECK(p.retry_after == 7);
    poller_finished(&p, true, -1);
    CHECK_INT(p.failures, 2);
    CHECK(p.retry_after == -1);
    poller_finished(&p, false, 30);
    CHECK_INT(p.failures, 0);
    CHECK(p.retry_after == -1);
    CHECK(!p.running);
}

static void test_a_stopped_poller_never_fires(void) {
    Poller p; memset(&p, 0, sizeof p);
    p.timer_id = 4;
    CHECK(!poller_fired(&p, 4));
    CHECK(!poller_fired(&p, 5));
    poller_stop(&p);
    CHECK(!p.running);
    poller_set_base(&p, 45000);
    CHECK_INT(p.base_ms, 45000);
}

// MARK: - Rows laid out without a window

static void test_session_row_lays_out_dot_title_and_subtitle(void) {
    ensure_theme();
    Session s = session_from("{\"id\":\"s1\",\"status\":\"waiting\",\"model\":\"opus\",\"title\":\"Fix the build\"}");
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 400);
    int box = doc_session_row(&doc, 0, 400, &s, 7, 42, false, theme.sidebar, 30);
    CHECK_INT(box, 0);
    CHECK_INT(doc.count, 4);
    Item *b = doc_item(&doc, box);
    CHECK_INT(b->action, 7);
    CHECK_INT(b->arg, 42);
    CHECK(b->hand);
    CHECK_INT(b->fill, theme.sidebar);
    CHECK_INT(b->rc.top, 0);
    CHECK_INT(b->rc.bottom, doc_height(&doc));
    CHECK_INT(doc_height(&doc), 7 + 23 + 2 + font_height(NULL, FONT_CAPTION) + 7);
    int title = item_with_text(&doc, "Fix the build");
    CHECK(title > box);
    if (title > 0) CHECK_INT(doc.items[title].rc.right, 8 + 400 - 16 - 30);
    CHECK(item_with_text(&doc, "Waiting \xC2\xB7 opus") > title);
    doc_end(&doc); doc_free(&doc);
    session_free(&s);
}

static void test_selected_session_row_is_raised(void) {
    ensure_theme();
    Session s = session_from("{\"id\":\"s1\",\"status\":\"idle\"}");
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 300);
    int box = doc_session_row(&doc, 10, 280, &s, 1, 0, true, theme.sidebar, 0);
    CHECK_INT(doc.items[box].fill, theme.raise);
    CHECK_INT(doc.items[box].border, theme.raise);
    CHECK_INT(doc.items[box].rc.left, 10);
    CHECK_INT(doc.items[box].rc.right, 290);
    CHECK(item_with_text(&doc, "New conversation") > box);
    CHECK(item_with_text(&doc, "Idle") > box);
    doc_free(&doc);
    session_free(&s);
}

static void test_pull_row_with_a_run_keeps_the_accent_border(void) {
    // #11: a pull request being worked on is drawn with the hover border.
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    p.number = 12; p.title = "Make it faster";
    Doc doc; doc_init(&doc);
    doc_begin(&doc, NULL, 600);
    doc_pull_row(&doc, 0, 600, &p, NULL, "o/r", 9, 12, NULL, 0, true);
    CHECK_INT(doc.items[0].border, theme.accent_dim);
    CHECK_INT(doc.items[0].fill, theme.raise);
    CHECK_INT(doc.items[0].action, 9);
    CHECK_INT(doc.items[0].arg, 12);
    CHECK_INT(doc.items[0].rc.bottom, doc_height(&doc));
    CHECK(item_with_text(&doc, "Make it faster") > 0);
    doc_begin(&doc, NULL, 600);
    doc_pull_row(&doc, 0, 600, &p, NULL, "o/r", 9, 12, NULL, 0, false);
    CHECK_INT(doc.items[0].border, theme.line);
    doc_free(&doc);
}

static void test_pull_row_grows_with_labels_and_buttons(void) {
    ensure_theme();
    PullSummary p; memset(&p, 0, sizeof p);
    p.number = 3; p.title = "T";
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600);
    doc_pull_row(&doc, 0, 600, &p, NULL, "o/r", 1, 0, NULL, 0, false);
    int bare = doc_height(&doc);
    PullLabel labels[2] = { { "bug", "d73a4a" }, { "ui", "00ff00" } };
    p.labels = labels; p.label_count = 2;
    ButtonSpec button = { 0xE768, "Run", BUTTON_BORDERED, 5, 0, true };
    doc_begin(&doc, NULL, 600);
    doc_pull_row(&doc, 0, 600, &p, NULL, "o/r", 1, 0, &button, 1, false);
    CHECK(doc_height(&doc) > bare);
    doc_free(&doc);
}

static void test_issue_row_names_its_parent_unless_nested(void) {
    ensure_theme();
    IssueSummary issue; memset(&issue, 0, sizeof issue);
    issue.number = 8; issue.title = "Child";
    issue.has_parent = true; issue.parent.number = 3; issue.parent.title = "Epic";
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600);
    doc_issue_row(&doc, 0, 600, &issue, "o/r", false, 2, 8);
    CHECK(item_with_text(&doc, "Child") > 0);
    CHECK(item_with_text(&doc, "Part of #3 Epic") > 0);
    CHECK_INT(doc.items[0].action, 2);
    int flat = doc_height(&doc);
    doc_begin(&doc, NULL, 600);
    doc_issue_row(&doc, 0, 600, &issue, "o/r", true, 2, 8);
    CHECK_INT(item_with_text(&doc, "Part of #3 Epic"), -1);
    CHECK(doc_height(&doc) < flat);
    issue.parent.repo = "other/repo";
    doc_begin(&doc, NULL, 600);
    doc_issue_row(&doc, 0, 600, &issue, "o/r", false, 2, 8);
    CHECK(item_with_text(&doc, "Part of other/repo#3 Epic") > 0);
    doc_free(&doc);
}

static void test_epic_issue_title_has_the_target_mark(void) {
    ensure_theme();
    IssueSummary issue; memset(&issue, 0, sizeof issue);
    issue.number = 3; issue.title = "Epic"; issue.sub_issues = 4; issue.sub_issues_done = 1;
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600);
    doc_issue_row(&doc, 0, 600, &issue, "o/r", false, 0, 0);
    CHECK(item_with_text(&doc, "\xE2\x97\x8E Epic") > 0);
    CHECK_INT(item_with_text(&doc, "Epic"), -1);
    doc_free(&doc);
}

static void test_label_chips_wrap_within_the_width(void) {
    ensure_theme();
    PullLabel labels[2] = { { "a-rather-long-label", "d73a4a" }, { "another-long-label", "00ff00" } };
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 600);
    doc_label_chips(&doc, 0, 600, labels, 0, theme.raise);
    CHECK_INT(doc.count, 0);
    CHECK_INT(doc_height(&doc), 0);
    doc_label_chips(&doc, 0, 600, labels, 2, theme.raise);
    CHECK_INT(doc.count, 1);
    int chip_h = font_height(NULL, FONT_CAPTION2) + 6;
    CHECK_INT(doc_height(&doc), chip_h);
    doc_begin(&doc, NULL, 40);
    doc_label_chips(&doc, 0, 40, labels, 2, theme.raise);
    CHECK_INT(doc_height(&doc), chip_h * 2 + 5);
    CHECK(doc.items[0].rc.right <= 40);
    doc_free(&doc);
}

static void test_tabs_run_left_to_right_and_wrap(void) {
    ensure_theme();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 1000);
    int x = 0, y = 0;
    doc_tab(&doc, &x, &y, 0, 1000, 40, 0xE8BD, "Conversation", NULL, true, 5, 1);
    int first = x;
    CHECK(first > 0);
    CHECK_INT(doc.items[0].rc.left, 0);
    CHECK_INT(doc.items[0].action, 0);
    CHECK(!doc.items[0].hover_fill);
    doc_tab(&doc, &x, &y, 0, 1000, 40, 0xE8BD, "Files", "12", false, 5, 2);
    CHECK_INT(doc.items[1].rc.left, first);
    CHECK_INT(doc.items[1].rc.top, 0);
    CHECK_INT(doc.items[1].action, 5);
    CHECK_INT(doc.items[1].arg, 2);
    CHECK_INT(y, 0);
    // A tab past the right edge starts a new line; the first one on a line never wraps however wide it is.
    doc_tab(&doc, &x, &y, 0, x + 10, 40, 0xE8BD, "Checks", NULL, false, 5, 3);
    CHECK_INT(doc.items[2].rc.left, 0);
    CHECK_INT(doc.items[2].rc.top, 40);
    CHECK_INT(y, 40);
    CHECK_INT(x, doc.items[2].rc.right);
    doc_tab(&doc, &x, &y, 0, 1, 40, 0xE8BD, "Findings", NULL, false, 5, 4);
    CHECK_INT(y, 80);
    int wide = x;
    x = 0;
    doc_tab(&doc, &x, &y, 0, 1, 40, 0xE8BD, "Findings", NULL, false, 5, 4);
    CHECK_INT(y, 80);
    CHECK_INT(x, wide);
    doc_free(&doc);
}

static void test_a_tab_count_makes_it_wider(void) {
    ensure_theme();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 1000);
    int x = 0, y = 0;
    doc_tab(&doc, &x, &y, 0, 1000, 40, 0xE8BD, "Files", NULL, false, 5, 1);
    int plain = x;
    x = 0;
    doc_tab(&doc, &x, &y, 0, 1000, 40, 0xE8BD, "Files", "3", false, 5, 1);
    CHECK(x > plain);
    doc_free(&doc);
}

void app_common_tests(void) {
    test_run("severity labels and colours", test_severity_labels_and_colours);
    test_run("unknown severity reads as medium", test_unknown_severity_reads_as_medium);
    test_run("severity colours are the ones the screens used before sharing", test_severity_colours_are_the_ones_the_screens_used_before_sharing);
    test_run("decisions are indexed by their api spelling", test_decisions_are_indexed_by_their_api_spelling);
    test_run("unknown decisions have no index", test_unknown_decisions_have_no_index);
    test_run("session subtitle is status and model", test_session_subtitle_is_status_and_model);
    test_run("set string replaces and clears", test_set_string_replaces_and_clears);
    test_run("people lists logins up to a limit", test_people_lists_logins_up_to_a_limit);
    test_run("label colour is github's hex or secondary", test_label_colour_is_githubs_hex_or_secondary);
    test_run("review glyphs and colours", test_review_glyphs_and_colours);
    test_run("check glyphs group conclusions", test_check_glyphs_group_conclusions);
    test_run("pending or unknown checks are amber", test_pending_or_unknown_checks_are_amber);
    test_run("tool glyphs follow the tool name", test_tool_glyphs_follow_the_tool_name);
    test_run("tool glyph kind and errors come first", test_tool_glyph_kind_and_errors_come_first);
    test_run("pull badges in order", test_pull_badges_in_order);
    test_run("pull badges stop at the capacity", test_pull_badges_stop_at_the_capacity);
    test_run("pull badges for check states", test_pull_badges_for_check_states);
    test_run("conflicts badge needs github's answer, not the label", test_conflicts_badge_needs_githubs_answer_not_the_label);
    test_run("pull badges review from the reviewers", test_pull_badges_review_from_the_reviewers);
    test_run("stack badge text is cut to its buffer", test_stack_badge_text_is_cut_to_its_buffer);
    test_run("poller counts failures and keeps retry-after", test_poller_counts_failures_and_keeps_retry_after);
    test_run("a stopped poller never fires", test_a_stopped_poller_never_fires);
    test_run("session row lays out dot, title and subtitle", test_session_row_lays_out_dot_title_and_subtitle);
    test_run("selected session row is raised", test_selected_session_row_is_raised);
    test_run("pull row with a run keeps the accent border", test_pull_row_with_a_run_keeps_the_accent_border);
    test_run("pull row grows with labels and buttons", test_pull_row_grows_with_labels_and_buttons);
    test_run("issue row names its parent unless nested", test_issue_row_names_its_parent_unless_nested);
    test_run("epic issue title has the target mark", test_epic_issue_title_has_the_target_mark);
    test_run("label chips wrap within the width", test_label_chips_wrap_within_the_width);
    test_run("tabs run left to right and wrap", test_tabs_run_left_to_right_and_wrap);
    test_run("a tab count makes it wider", test_a_tab_count_makes_it_wider);
}
