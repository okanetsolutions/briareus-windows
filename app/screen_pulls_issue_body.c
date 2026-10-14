#include "screen_pulls_internal.h"

static bool issue_runs_cost(const IssueScreen *s, double *total);
static const char *issue_state(const IssueScreen *s, COLORREF *color);
static void issue_layout_header(IssueScreen *s, Doc *doc, Col c);

bool screen_pulls_issue_run_active(const IssueScreen *s) { for (size_t i = 0; i < s->run_count; i++) if (session_is_active(&s->runs[i])) return true; return false; }

/// What the conversations on this issue spent, each counted with the workers it ordered. False when none was priced.
static bool issue_runs_cost(const IssueScreen *s, double *total) {
    bool any = false; *total = 0;
    for (size_t i = 0; i < s->run_count; i++) {
        double c;
        if (json_num(json_get(json_get(s->runs[i].raw, "usage"), "costUsd"), &c) || json_num(json_get(s->runs[i].raw, "costUsd"), &c)) { *total += c; any = true; }
    }
    return any;
}

/// The pull requests still open among those linked to it; the full read lists merged and closed ones too.
size_t screen_pulls_issue_open_pulls(const IssueScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->issue.pull_count; i++) if (!s->issue.pulls[i].state || str_eq(s->issue.pulls[i].state, "open")) n++;
    return n;
}

/// An issue of this repository opens here, from its board row when it has one; any other on GitHub.
void screen_pulls_issue_open_link(IssueScreen *s, const BoardLink *link) {
    if (!board_link_is_foreign(link, s->project.repo)) {
        const IssueSummary *row = issues_find(s->board_issues, s->board_issue_count, link->number);
        if (row) { app_push_from(&s->base, issue_detail_screen_new(&s->project, row)); return; }
        if (store_supports("issue")) {
            IssueSummary bare; memset(&bare, 0, sizeof bare);
            bare.number = link->number; bare.title = link->title; bare.url = link->url;
            app_push_from(&s->base, issue_detail_screen_new(&s->project, &bare));
            return;
        }
    }
    open_web_url(link->url);
}

void screen_pulls_issue_open_pull(IssueScreen *s, const BoardLink *link) {
    if (board_link_is_foreign(link, s->project.repo) || !store_supports("pull")) open_web_url(link->url);
    else app_push_from(&s->base, pull_detail_screen_new(&s->project, link->number, NULL, pulls_find(s->board_pulls, s->board_pull_count, link->number)));
}

/// The state pill: GitHub's green Open, purple Closed, and grey for one closed as not planned or a duplicate.
static const char *issue_state(const IssueScreen *s, COLORREF *color) {
    const char *state = json_str(json_get(s->detail, "state")), *reason = json_str(json_get(s->detail, "stateReason"));
    if (s->closed) { state = "closed"; reason = s->closed_reason; }
    if (str_eq(state, "closed")) { *color = str_eq(reason, "not_planned") || str_eq(reason, "duplicate") ? theme.muted : theme.accent; return "closed"; }
    // Off the board and not read on its own (yet): it may well be closed.
    if (!s->detail && s->gone) { *color = theme.muted; return "not on the board"; }
    *color = theme.ok;
    return "open";
}

bool screen_pulls_issue_is_closed(const IssueScreen *s) { COLORREF c; return str_eq(issue_state(s, &c), "closed"); }

bool screen_pulls_issue_editable(const IssueScreen *s) { return store_supports("update_issue") && !s->closed; }

/// The title with its number, then the state pill and GitHub's sentence beside it: who opened it, when, its comments.
static void issue_layout_header(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    ButtonSpec buttons[2]; size_t bn = 0;
    // The description is the issue's own read; without it an edit would start from nothing.
    if (screen_pulls_issue_editable(s)) { ButtonSpec b = { 0, s->editing ? "Saving\xE2\x80\xA6" : "Edit", BUTTON_BORDERED, ACT_ISSUE_EDIT, 0, !s->editing && json_str(json_get(s->detail, "body")) }; buttons[bn++] = b; }
    if (safe_web_url(issue->url)) { ButtonSpec b = { 0, "Open in GitHub \xE2\x86\x97", BUTTON_BORDERED, ACT_ISSUE_OPEN, 0, true }; buttons[bn++] = b; }
    int tw = bn ? screen_pulls_toolbar_width(doc->cv, buttons, bn) + px(2) : 0;
    bool beside = !bn || c.iw - tw - px(16) >= px(320);
    int top = doc->y, buttons_bottom = top;
    if (bn && beside) { doc->y = top + px(3); doc_button_row(doc, c.ix + c.iw - tw, tw, buttons, bn); buttons_bottom = doc->y; doc->y = top; }
    else if (bn) { doc_button_row(doc, c.ix, c.iw, buttons, bn); doc_space(doc, px(12)); }
    FlowData *t = screen_pulls_flow_new(font_height(doc->cv, FONT_TITLE) + px(6));
    char *spaced = xstrfmt("%s ", issue->title); screen_pulls_flow_add(t, spaced, FONT_TITLE, theme.ink, false); free(spaced);
    char *number = xstrfmt("#%d", issue->number); screen_pulls_flow_add(t, number, FONT_TITLE, theme.muted, false); free(number);
    screen_pulls_doc_flow(doc, c.ix, bn && beside ? c.iw - tw - px(16) : c.iw, 0, t);
    if (doc->y < buttons_bottom) doc->y = buttons_bottom;
    doc_space(doc, px(10));
    COLORREF color;
    const char *state = issue_state(s, &color);
    int y = doc->y, pw = screen_pulls_doc_pill(doc, c.ix, y, state, color);
    FlowData *f = screen_pulls_flow_new(px(26));
    if (issue->author) { char *a = xstrfmt("%s ", issue->author); screen_pulls_flow_add(f, a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(a); }
    if (issue->has_created) {
        char *rel = format_relative(issue->created_at), *o = xstrfmt("%s %s ", issue->author ? "opened this issue" : "Opened", rel);
        screen_pulls_flow_add(f, o, FONT_FOOTNOTE, theme.muted, false);
        free(o); free(rel);
    }
    time_t closed_at;
    if (str_eq(state, "closed") && board_date_parse(json_str(json_get(s->detail, "closedAt")), &closed_at)) {
        char *rel = format_relative(closed_at), *t2 = xstrfmt("\xC2\xB7 closed %s ", rel);
        screen_pulls_flow_add(f, t2, FONT_FOOTNOTE, theme.muted, false);
        free(t2); free(rel);
    }
    if (issue->comments > 0) { char *n = xstrfmt("\xC2\xB7 %d comment%s", issue->comments, issue->comments == 1 ? "" : "s"); screen_pulls_flow_add(f, n, FONT_FOOTNOTE, theme.muted, false); free(n); }
    if (f->count) screen_pulls_doc_flow(doc, c.ix + pw + px(10), c.iw - pw - px(10), 0, f); else screen_pulls_flow_free(f);
    if (doc->y < y + px(26)) doc->y = y + px(26);
}

/// The opening comment: the issue's body as GitHub shows it.
void screen_pulls_issue_layout_body(IssueScreen *s, Doc *doc, Col c) {
    if (!store_supports("issue")) return;
    const IssueSummary *issue = &s->issue;
    CommentHeadData *h = xcalloc(1, sizeof *h);
    if (issue->author) h->author = xstrdup(issue->author);
    if (issue->has_created) { char *rel = format_relative(issue->created_at); h->when = xstrfmt("%s \xC2\xB7 %s", issue->author ? "opened" : "Description", rel); free(rel); }
    else h->when = xstrdup(issue->author ? "opened" : "Description");
    int ix, iw, box = screen_pulls_comment_begin(doc, c, h, 0, 0, &ix, &iw);
    const char *body = json_str(json_get(s->detail, "body"));
    if (!s->detail) doc_text(doc, ix, iw, s->detail_error ? "The description could not be read." : "Loading the description\xE2\x80\xA6", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
    else {
        char *trimmed = visible_markdown(body ? body : "");
        if (*trimmed) doc_markdown(doc, ix, iw, trimmed, FONT_CALLOUT);
        else doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
        free(trimmed);
    }
    doc_box_end(doc, box, px(16));
}

/// An epic's sub-issues: the open ones on the board drawn as the board draws them, every other one as a link.
void screen_pulls_issue_layout_subs(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    if (!issue_is_epic(issue)) return;
    int open = issue->sub_issues - issue->sub_issues_done;
    doc_section(doc, c.x, c.w, "Sub-issues");
    char *t = xstrfmt("%d of %d completed", issue->sub_issues_done, issue->sub_issues);
    doc_text(doc, c.x, c.w, t, FONT_CAPTION, open > 0 ? theme.secondary : theme.success, DT_SINGLELINE);
    free(t);
    doc_space(doc, px(4));
    doc_epic_progress(doc, c.x, c.w, issue);
    doc_space(doc, px(10));
    size_t sn; size_t *rich = issue_open_sub_issues(s->board_issues, s->board_issue_count, issue->number, s->project.repo, &sn);
    for (size_t k = 0; k < sn; k++) {
        doc_issue_row(doc, c.x, c.w, &s->board_issues[rich[k]], s->project.repo, true, ACT_ISSUE_SUB, (intptr_t)rich[k]);
        doc_space(doc, px(8));
    }
    // The full read lists them all; those not drawn above go in one box of links, closed ones included.
    size_t *rest = xcalloc(s->sub_count ? s->sub_count : 1, sizeof *rest), rn = 0;
    for (size_t i = 0; i < s->sub_count; i++) {
        bool drawn = false;
        for (size_t k = 0; k < sn && !drawn; k++) drawn = !board_link_is_foreign(&s->subs[i], s->project.repo) && s->board_issues[rich[k]].number == s->subs[i].number;
        if (!drawn) rest[rn++] = i;
    }
    if (rn) {
        int box = screen_pulls_col_box(doc, c);
        for (size_t k = 0; k < rn; k++) {
            const BoardLink *link = &s->subs[rest[k]];
            if (k) doc_space(doc, px(4));
            doc_linked_row(doc, c.ix, c.iw, link, s->project.repo, safe_web_url(link->url) || store_supports("issue") ? ACT_ISSUE_SUB_LINK : 0, (intptr_t)rest[k]);
        }
        doc_box_end(doc, box, px(12));
    } else if (!sn) {
        int box = screen_pulls_col_box(doc, c);
        const char *m = open <= 0 ? "Every sub-issue is closed. The epic itself stays open until it is closed on GitHub."
                      : !s->board_read && !s->detail ? "Reading the board\xE2\x80\xA6"
                      : "None of its open sub-issues is on this project\xE2\x80\x99s board.";
        doc_text(doc, c.ix, c.iw, m, FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, box, px(12));
    }
    // Without the full read, the board is all there is: say what it cannot show.
    if (!s->detail && open > 0 && (int)sn < open && s->board_read) {
        int missing = open - (int)sn;
        char *m = xstrfmt("%d open sub-issue%s %s in another repository or past the issues the board reads; GitHub lists them all.", missing, missing == 1 ? "" : "s", missing == 1 ? "is" : "are");
        doc_space(doc, px(6));
        doc_text(doc, c.x, c.w, m, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        free(m);
    }
    free(rest); free(rich);
}

void screen_pulls_issue_layout_runs(IssueScreen *s, Doc *doc, Col c) {
    if (!s->run_count) return;
    doc_section(doc, c.x, c.w, "Sessions");
    for (size_t i = 0; i < s->run_count; i++) {
        doc_session_row(doc, c.x, c.w, &s->runs[i], ACT_ISSUE_RUN, (intptr_t)i, false, theme.raise, 0);
        doc_space(doc, px(6));
    }
    double cost;
    if (issue_runs_cost(s, &cost)) {
        char *cc = format_cost(cost);
        char *t = xstrfmt("%s spent across %zu session%s, their workers included", cc, s->run_count, s->run_count == 1 ? "" : "s");
        doc_text(doc, c.x, c.w, t, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        free(t); free(cc);
    }
}

void screen_pulls_issue_layout(Screen *base, Doc *doc) {
    IssueScreen *s = (IssueScreen *)base;
    int w = doc->width, ix = px(12), iw = w - px(24);
    doc_space(doc, px(14));
    if (s->write_error) {
        int b = screen_pulls_section_box(doc, w);
        doc_notice(doc, ix, iw, s->write_error);
        if (s->uncertain) {
            doc_space(doc, px(6));
            doc_text(doc, ix, iw, "The request may have completed. Check the project\xE2\x80\x99s conversations before starting another agent.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
            doc_space(doc, px(8));
            doc_button(doc, ix, 0, "I have checked", BUTTON_BORDERED, ACT_ISSUE_CHECKED, 0, true);
        }
        doc_box_end(doc, b, px(12));
        doc_space(doc, px(10));
    }
    if (s->edit_error) { doc_notice(doc, 0, w, s->edit_error); doc_space(doc, px(10)); }
    if (s->detail_error) { doc_notice(doc, 0, w, s->detail_error); doc_space(doc, px(10)); }
    if (s->load_error && !str_eq(s->load_error, s->detail_error)) { doc_notice(doc, 0, w, s->load_error); doc_space(doc, px(10)); }
    if (s->closed) {
        int b = screen_pulls_section_box(doc, w);
        doc_label(doc, ix, iw, 0xE73E, str_eq(s->closed_reason, "not_planned") ? "Closed as not planned" : "Closed as completed", FONT_CALLOUT, theme.success);
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "It has left the board. Reopen it on GitHub if it was closed by mistake.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, b, px(12));
        doc_space(doc, px(10));
    } else if (s->gone && !s->detail) {
        int b = screen_pulls_section_box(doc, w);
        doc_label(doc, ix, iw, 0xE946, "This issue is no longer on the board", FONT_CALLOUT, theme.text);
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "It was closed, or it is past the most recently updated issues the server reads. What is shown is how it was last seen.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, b, px(12));
        doc_space(doc, px(10));
    }
    Col head = { 0, w, px(4), w - px(8) };
    issue_layout_header(s, doc, head);
    doc_space(doc, px(18));
    if (w >= px(880)) {
        // Wide: the issue and its timeline on the left, GitHub's sidebar on the right.
        int side_w = w * 26 / 100, gap = px(28);
        if (side_w < px(240)) side_w = px(240);
        if (side_w > px(320)) side_w = px(320);
        Col main = screen_pulls_col_make(0, w - side_w - gap), side = { w - side_w, side_w, w - side_w, side_w };
        int top = doc->y;
        screen_pulls_issue_layout_main(s, doc, main);
        int main_bottom = doc->y;
        doc->y = top - px(14);
        screen_pulls_issue_layout_sidebar(s, doc, side);
        if (doc->y < main_bottom) doc->y = main_bottom;
    } else {
        screen_pulls_issue_layout_main(s, doc, screen_pulls_col_make(0, w));
        doc_space(doc, px(6));
        Col side = { px(4), w - px(8), px(4), w - px(8) };
        screen_pulls_issue_layout_sidebar(s, doc, side);
    }
    doc_space(doc, px(16));
}

void screen_pulls_issue_header(Screen *base, HeaderInfo *info) {
    IssueScreen *s = (IssueScreen *)base;
    snprintf(info->title, sizeof info->title, "#%d", s->issue.number);
    time_t retry = pulls_retry_deadline(s->project.repo);
    if (retry > time(NULL)) { char *at = format_event_time(retry); snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 list retry after %s", s->project.repo, at); free(at); }
    else snprintf(info->subtitle, sizeof info->subtitle, "%s", s->project.repo);
    HeaderButton *b = &info->buttons[info->button_count++]; b->glyph = 0xE8C8; b->action = ACT_ISSUE_COPY; b->enabled = safe_web_url(s->issue.url); b->tip = "Copy the issue\xE2\x80\x99s link";
    b = &info->buttons[info->button_count++]; b->glyph = 0xE72C; b->action = ACT_ISSUE_REFRESH; b->enabled = !s->req_board && !s->req_detail; b->tip = "Read the issue from GitHub again";
}
