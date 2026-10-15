#include "screen_pulls_internal.h"

static void issue_side_actions(IssueScreen *s, Doc *doc, Col c, int *count);
static void issue_side_projects(IssueScreen *s, Doc *doc, Col c, int *count);

// The sidebar, as GitHub's: what can be done here, then who and what it is filed under.

static void issue_side_actions(IssueScreen *s, Doc *doc, Col c, int *count) {
    if (screen_pulls_issue_is_closed(s)) return;
    bool start = store_supports("start_session"), close = store_supports("close_issue");
    if (!start && !close) return;
    screen_pulls_side_heading(doc, c, count, s->busy ? "Actions \xC2\xB7 starting\xE2\x80\xA6" : "Actions");
    const IssueSummary *issue = &s->issue;
    if (start && issue_is_epic(issue)) {
        doc_text(doc, c.x, c.w, "An epic is worked by an orchestrator, one sub-issue at a time. Start it from the web dashboard, where its models are picked, or start one of its sub-issues here.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    } else if (start) {
        doc_button(doc, c.x, c.w, s->busy ? "Starting\xE2\x80\xA6" : "Start a session on this issue", BUTTON_PROMINENT, ACT_ISSUE_START, 0, !s->busy && !s->uncertain);
        doc_space(doc, px(8));
        doc_text(doc, c.x, c.w, screen_pulls_issue_run_active(s) ? "A session is already working on this issue; it is listed under Sessions. A second one is a paid agent doing the same work."
                              : screen_pulls_issue_open_pulls(s) ? "A pull request is already answering this issue. A second session is a paid agent working on the same thing."
                                                    : "The session reads the issue, implements it on a branch of its own and opens a pull request closing it. It runs a paid agent on this project\xE2\x80\x99s configured model.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
    if (close) {
        if (start) doc_space(doc, px(12));
        doc_button(doc, c.x, c.w, s->closing ? "Closing\xE2\x80\xA6" : "Close issue \xE2\x96\xBE", BUTTON_BORDERED, ACT_ISSUE_CLOSE, 0, !s->closing);
        doc_space(doc, px(8));
        doc_text(doc, c.x, c.w, screen_pulls_issue_open_pulls(s) ? "Closes it on GitHub now. The pull requests answering it stay open; merging one later will not close it again."
                                                    : "Closes it on GitHub, as completed or as not planned, with a comment first if you write one.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
}

static void issue_side_projects(IssueScreen *s, Doc *doc, Col c, int *count) {
    if (!s->detail) return;
    const Json *projects = json_get(s->detail, "projects");
    screen_pulls_side_heading(doc, c, count, "Projects");
    for (size_t i = 0; i < json_count(projects); i++) { if (i) doc_space(doc, px(10)); screen_pulls_side_project(doc, c, json_at(projects, i), NULL, ACT_ISSUE_PROJECT, (intptr_t)i); }
    if (!json_count(projects)) screen_pulls_side_projects_none(doc, c, json_str(json_get(s->detail, "projectsError")));
}

void screen_pulls_issue_layout_sidebar(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    int count = 0;
    issue_side_actions(s, doc, c, &count);
    screen_pulls_side_heading(doc, c, &count, "Assignees");
    for (size_t i = 0; i < issue->assignee_count; i++) { if (i) doc_space(doc, px(4)); doc_text(doc, c.x, c.w, issue->assignees[i], FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS); }
    if (!issue->assignee_count) doc_text(doc, c.x, c.w, "No one", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (screen_pulls_issue_editable(s)) screen_pulls_side_edit_button(doc, c, "Edit assignees \xE2\x96\xBE", ACT_ISSUE_ASSIGNEES, !s->editing);
    screen_pulls_side_heading(doc, c, &count, "Labels");
    if (issue->label_count) doc_label_chips(doc, c.x, c.w, issue->labels, issue->label_count, theme.canvas);
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (screen_pulls_issue_editable(s)) screen_pulls_side_edit_button(doc, c, "Edit labels\xE2\x80\xA6", ACT_ISSUE_LABELS, !s->editing);
    if (s->detail) {
        const char *type = json_str(json_get(s->detail, "type"));
        screen_pulls_side_heading(doc, c, &count, "Type");
        doc_text(doc, c.x, c.w, type ? type : "No type", type ? FONT_FOOTNOTE : FONT_CAPTION, type ? theme.ink : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    issue_side_projects(s, doc, c, &count);
    screen_pulls_side_heading(doc, c, &count, "Milestone");
    doc_text(doc, c.x, c.w, issue->milestone ? issue->milestone : "No milestone", issue->milestone ? FONT_FOOTNOTE : FONT_CAPTION, issue->milestone ? theme.ink : theme.secondary, DT_WORDBREAK);
    screen_pulls_side_heading(doc, c, &count, "Relationships");
    if (issue->has_parent) {
        doc_text(doc, c.x, c.w, "Parent issue", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
        doc_space(doc, px(2));
        bool linked = !board_link_is_foreign(&issue->parent, s->project.repo) || safe_web_url(issue->parent.url);
        doc_linked_row(doc, c.x, c.w, &issue->parent, s->project.repo, linked ? ACT_ISSUE_PARENT : 0, 0);
    } else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    screen_pulls_side_heading(doc, c, &count, "Development");
    size_t open_pulls = screen_pulls_issue_open_pulls(s);
    if (open_pulls) {
        doc_text(doc, c.x, c.w, open_pulls == 1 ? "Successfully merging this pull request may close this issue." : "Successfully merging one of these pull requests may close this issue.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(6));
    } else if (!issue->pull_count) doc_text(doc, c.x, c.w, "No pull request yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    for (size_t i = 0; i < issue->pull_count; i++) {
        const BoardLink *pull = &issue->pulls[i];
        bool foreign = board_link_is_foreign(pull, s->project.repo) || !store_supports("pull");
        if (i) doc_space(doc, px(2));
        doc_linked_row(doc, c.x, c.w, pull, s->project.repo, foreign ? (safe_web_url(pull->url) ? ACT_ISSUE_PULL : 0) : ACT_ISSUE_PULL, (intptr_t)i);
    }
    if (issue->has_updated) {
        screen_pulls_side_heading(doc, c, &count, "Updated");
        char *rel = format_relative(issue->updated_at); doc_text(doc, c.x, c.w, rel, FONT_FOOTNOTE, theme.ink, DT_SINGLELINE); free(rel);
    }
}
