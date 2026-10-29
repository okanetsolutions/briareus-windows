#include "screen_pulls_internal.h"

static void side_field(Doc *doc, Col c, const char *name, const char *value);
static void side_actions(PullScreen *s, Doc *doc, Col c, int *count);
static void side_checks(PullScreen *s, Doc *doc, Col c, int *count);
static void side_reviewer(Doc *doc, Col c, const char *user, const BadgeSpec *badge, const char *state);
static void side_reviewers(PullScreen *s, Doc *doc, Col c, int *count);
static void side_assignees(PullScreen *s, Doc *doc, Col c, int *count);
static void side_milestone(PullScreen *s, Doc *doc, Col c, int *count);
static void side_labels(PullScreen *s, Doc *doc, Col c, int *count);
static void side_development(PullScreen *s, Doc *doc, Col c, int *count);
static void side_projects(PullScreen *s, Doc *doc, Col c, int *count);

/// `.discussion-sidebar-item`: a small heading over its contents, a rule between items.
void screen_pulls_side_heading(Doc *doc, Col c, int *count, const char *title) {
    if ((*count)++) { doc_space(doc, px(14)); doc_rule(doc, c.x, c.w); }
    doc_space(doc, px(14));
    doc_text(doc, c.x, c.w, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(8));
}

/// The edit button under the sidebar's assignees or labels.
void screen_pulls_side_edit_button(Doc *doc, Col c, const char *text, int action, bool enabled) {
    doc_space(doc, px(8));
    ButtonSpec b = { 0, text, BUTTON_BORDERED, action, 0, enabled };
    doc_button_row(doc, c.x, c.w, &b, 1);
}

/// A name on the left and its value on the right, as a project's fields are listed.
static void side_field(Doc *doc, Col c, const char *name, const char *value) {
    int y = doc->y, half = c.w * 2 / 5;
    RECT nr = { c.x, y, c.x + half - px(6), y + px(20) }; doc_text_at(doc, &nr, name, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = y;
    doc_text(doc, c.x + half, c.w - half, value, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
    if (doc->y < y + px(20)) doc->y = y + px(20);
    doc_space(doc, px(2));
}

/// One Projects v2 board an issue is on: its title (with `from`, the issue it came through, when given), which `action`
/// opens on GitHub, then its Status and the rest of its fields.
void screen_pulls_side_project(Doc *doc, Col c, const Json *p, const char *from, int action, intptr_t arg) {
    const char *title = json_str(json_get(p, "title")), *status = json_str(json_get(p, "status"));
    char *label = from ? xstrfmt("%s \xC2\xB7 %s", title ? title : "Project", from) : NULL;
    int li = doc_label(doc, c.x, c.w, 0xE8FD, label ? label : title ? title : "Project", FONT_FOOTNOTE_SEMIBOLD, theme.ink);
    free(label);
    if (safe_web_url(json_str(json_get(p, "url")))) { doc_item(doc, li)->action = action; doc_item(doc, li)->arg = arg; doc_item(doc, li)->hand = true; }
    doc_space(doc, px(4));
    side_field(doc, c, "Status", status ? status : "No status");
    const Json *fields = json_get(p, "fields");
    for (size_t k = 0; k < json_count(fields); k++) {
        const Json *field = json_at(fields, k), *v = json_get(field, "value");
        const char *name = json_str(json_get(field, "name"));
        double n;
        char *value = json_str(v) ? xstrdup(json_str(v)) : json_num(v, &n) ? xstrfmt("%g", n) : NULL;
        if (name && value) side_field(doc, c, name, value);
        free(value);
    }
}

/// What the Projects item says when there are none: why, when GitHub would not read them.
void screen_pulls_side_projects_none(Doc *doc, Col c, const char *refused) {
    if (refused) { char *t = xstrfmt("GitHub would not read its projects with the server\xE2\x80\x99s token, which needs Projects: read. %s", refused); doc_text(doc, c.x, c.w, t, FONT_CAPTION, theme.secondary, DT_WORDBREAK); free(t); }
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
}

/// The errands as the sidebar's first item: one full-width button per action, the one the pull request's state asks for filled.
static void side_actions(PullScreen *s, Doc *doc, Col c, int *count) {
    if (!s->action_count) return;
    const PullSummary *row = screen_pulls_board_row(s);
    screen_pulls_side_heading(doc, c, count, s->busy ? "Actions \xC2\xB7 starting\xE2\x80\xA6" : "Actions");
    bool enabled = !s->busy && !s->uncertain;
    for (size_t i = 0; i < s->action_count; i++) {
        const BoardAction *a = &s->actions[i];
        bool suggested = row && str_eq(row->recommended, a->id);
        char *label = xstrfmt("%s %s", screen_pulls_action_icon(a->id), a->label);
        if (i) doc_space(doc, px(6));
        doc_button(doc, c.x, c.w, label, suggested ? BUTTON_PROMINENT : str_eq(a->id, "delete-self-comments") ? BUTTON_DESTRUCTIVE : BUTTON_BORDERED, ACT_START_ACTION, (intptr_t)i, enabled);
        free(label);
    }
    doc_space(doc, px(8));
    doc_text(doc, c.x, c.w, "Actions run paid agents and may write to GitHub. CLI-restricted actions ask for a compatible provider, model and effort.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
}

/// The checks as counts only: passed, pending and failed.
static void side_checks(PullScreen *s, Doc *doc, Col c, int *count) {
    if (json_is_null(s->pr)) return;
    const Json *checks = json_get(s->pr, "checks");
    screen_pulls_side_heading(doc, c, count, "Checks");
    if (!json_count(json_get(checks, "runs"))) { doc_text(doc, c.x, c.w, "No checks reported", FONT_CAPTION, theme.secondary, DT_SINGLELINE); return; }
    CountsData *cd = xcalloc(1, sizeof *cd);
    cd->passed = json_int_or(json_get(checks, "passed"), 0); cd->pending = json_int_or(json_get(checks, "pending"), 0); cd->failed = json_int_or(json_get(checks, "failed"), 0);
    int i = doc_custom(doc, c.x, c.w, px(22), screen_pulls_paint_counts, cd, free, 0, 0);
    doc_item(doc, i)->hover_fill = false;
}

static void side_reviewer(Doc *doc, Col c, const char *user, const BadgeSpec *badge, const char *state) {
    int y = doc->y;
    RECT ur = { c.x, y, c.x + c.w / 2, y + px(22) }; doc_text_at(doc, &ur, user, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (badge) { int bw = draw_badge(NULL, 0, 0, badge->glyph, badge->text, badge->color, theme.canvas, NULL); doc->y = y; doc_badges(doc, c.x + c.w - bw, bw, badge, 1, theme.canvas); }
    else if (state) { RECT sr = { c.x + c.w / 2, y, c.x + c.w, y + px(22) }; doc_text_at(doc, &sr, state, FONT_CAPTION, theme.secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
    if (doc->y < y + px(22)) doc->y = y + px(22);
    doc_space(doc, px(4));
}

static void side_reviewers(PullScreen *s, Doc *doc, Col c, int *count) {
    if (json_is_null(s->pr)) return;
    const PullSummary *row = screen_pulls_board_row(s);
    screen_pulls_side_heading(doc, c, count, "Reviewers");
    const Json *reviews = json_get(s->pr, "reviews");
    size_t listed = 0;
    for (size_t i = 0; i < json_count(reviews); i++) {
        const Json *review = json_at(reviews, i);
        const char *user = json_str(json_get(review, "user"));
        Json *one = json_array(); json_array_push(one, json_clone(review));
        ReviewStatus st = review_status(NULL, one); json_free(one);
        if (st != REVIEW_NONE) { COLORREF cc; wchar_t g = review_glyph(st, &cc); BadgeSpec b = { g, review_status_text(st), cc, false }; side_reviewer(doc, c, user ? user : "Reviewer", &b, NULL); }
        else side_reviewer(doc, c, user ? user : "Reviewer", NULL, json_str(json_get(review, "state")));
        listed++;
    }
    for (size_t i = 0; row && i < row->reviewer_count; i++) {
        const Reviewer *r = &row->reviewers[i];
        if (!str_eq(r->state, "requested")) continue;
        bool reviewed = false;
        for (size_t k = 0; k < json_count(reviews) && !reviewed; k++) reviewed = str_ieq(json_str(json_get(json_at(reviews, k), "user")), r->user);
        if (reviewed) continue;
        BadgeSpec b = { 0xE823, "Review requested", theme.secondary, false };
        side_reviewer(doc, c, r->user, &b, NULL);
        listed++;
    }
    if (!listed) doc_text(doc, c.x, c.w, "No reviews", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
}

static void side_assignees(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = screen_pulls_board_row(s);
    if (!row) return;
    screen_pulls_side_heading(doc, c, count, "Assignees");
    for (size_t i = 0; i < row->assignee_count; i++) { if (i) doc_space(doc, px(4)); doc_text(doc, c.x, c.w, row->assignees[i], FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS); }
    if (!row->assignee_count) doc_text(doc, c.x, c.w, "No one", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (store_supports("update_pull") && screen_pulls_is_open(s)) screen_pulls_side_edit_button(doc, c, "Edit assignees \xE2\x96\xBE", ACT_EDIT_ASSIGNEES, !s->editing);
}

static void side_milestone(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = screen_pulls_board_row(s);
    const char *milestone = row ? json_str(json_get(row->raw, "milestone")) : NULL;
    if (!milestone) return;
    screen_pulls_side_heading(doc, c, count, "Milestone");
    doc_text(doc, c.x, c.w, milestone, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
}

static void side_labels(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = screen_pulls_board_row(s);
    if (!row) return;
    screen_pulls_side_heading(doc, c, count, "Labels");
    if (row->label_count) doc_label_chips(doc, c.x, c.w, row->labels, row->label_count, theme.canvas);
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (store_supports("update_pull") && screen_pulls_is_open(s)) screen_pulls_side_edit_button(doc, c, "Edit labels\xE2\x80\xA6", ACT_EDIT_LABELS, !s->editing);
}

static void side_development(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = screen_pulls_board_row(s);
    const BoardLink *issues = row && row->issue_count ? row->issues : NULL; size_t issue_count = row ? row->issue_count : 0;
    BoardLink *parsed = NULL; size_t parsed_count = 0;
    if (!issues) {
        const Json *arr = json_get(s->pr, "issues");
        parsed = xcalloc(json_count(arr) ? json_count(arr) : 1, sizeof *parsed);
        for (size_t i = 0; i < json_count(arr); i++) if (board_link_parse(json_at(arr, i), &parsed[parsed_count])) parsed_count++;
        issues = parsed; issue_count = parsed_count;
    }
    if (issue_count) {
        screen_pulls_side_heading(doc, c, count, "Development");
        doc_text(doc, c.x, c.w, "Successfully merging this pull request may close these issues.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(6));
        for (size_t i = 0; i < issue_count; i++) { if (i) doc_space(doc, px(2)); doc_linked_row(doc, c.x, c.w, &issues[i], s->project.repo, safe_web_url(issues[i].url) || (!board_link_is_foreign(&issues[i], s->project.repo) && store_supports("issue")) ? ACT_ISSUE_URL : 0, (intptr_t)i); }
    }
    for (size_t i = 0; i < parsed_count; i++) board_link_free(&parsed[i]);
    free(parsed);
}

/// The projects of the issues it closes, which is where a team keeps its board fields; named after their issue when it
/// closes more than one.
static void side_projects(PullScreen *s, Doc *doc, Col c, int *count) {
    size_t issues = json_count(s->issue_projects), shown = 0;
    if (!issues) return;
    screen_pulls_side_heading(doc, c, count, "Projects");
    const char *refused = NULL;
    for (size_t i = 0; i < issues; i++) {
        const Json *e = json_at(s->issue_projects, i), *projects = json_get(e, "projects");
        if (!refused) refused = json_str(json_get(e, "projectsError"));
        char *from = issues > 1 ? xstrfmt("#%d", json_int_or(json_get(e, "number"), 0)) : NULL;
        for (size_t k = 0; k < json_count(projects); k++) {
            if (shown++) doc_space(doc, px(10));
            screen_pulls_side_project(doc, c, json_at(projects, k), from, ACT_PR_PROJECT, (intptr_t)(i << 16 | k));
        }
        free(from);
    }
    if (!shown) screen_pulls_side_projects_none(doc, c, refused);
}

/// The sidebar: only what the server reports; the projects are those of the issues it closes, and notifications are not
/// part of it.
void screen_pulls_layout_sidebar(PullScreen *s, Doc *doc, Col c) {
    int count = 0;
    side_actions(s, doc, c, &count);
    side_checks(s, doc, c, &count);
    side_reviewers(s, doc, c, &count);
    side_assignees(s, doc, c, &count);
    side_labels(s, doc, c, &count);
    side_projects(s, doc, c, &count);
    side_milestone(s, doc, c, &count);
    side_development(s, doc, c, &count);
}
