#include "screen_global_pulls_internal.h"

bool global_board_incomplete(const GlobalPullsScreen *s, GlobalBoardTab tab) {
    if (!s->listed || s->loading || s->error || s->limit < s->state.count) return true;
    for (size_t i = 0; i < s->state.count; i++) {
        const GlobalPullProject *p = &s->state.projects[i];
        if (!p->loaded || p->error) return true;
        if (tab == GLOBAL_TAB_ISSUES ? p->issues_error || p->issues_truncated : p->truncated) return true;
    }
    return false;
}
void global_pulls_layout(Screen *base, Doc *doc) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base; int w = doc->width;
    if (!global_pulls_current(s)) { doc_notice(doc, 0, w, "Reopen All projects with access to project pull requests."); return; }
    global_board_tabs(s, doc);
    if (s->error) { doc_notice(doc, 0, w, s->error); doc_space(doc, px(8)); }
    if (global_board_incomplete(s, s->tab)) {
        doc_notice(doc, 0, w, "Scope incomplete. Counts cover loaded snapshots only; unavailable or unread projects may have more rows."); doc_space(doc, px(10));
    }
    size_t end = s->state.count < s->limit ? s->state.count : s->limit;
    for (size_t i = 0; i < end; i++) {
        GlobalPullProject *p = &s->state.projects[i];
        const char *issue_error = s->tab == GLOBAL_TAB_ISSUES ? p->issues_error : NULL;
        bool truncated = s->tab == GLOBAL_TAB_ISSUES ? p->issues_truncated : p->truncated;
        if (p->error || issue_error || truncated) {
            const char *reason = p->error ? p->error : issue_error ? issue_error : "Only the most recently updated rows are included; open this project for more context.";
            char *note = xstrfmt("%s: %s", p->project.repo, reason);
            doc_notice(doc, 0, w, note); free(note);
            time_t retry = pulls_retry_deadline(p->project.repo);
            if (retry > time(NULL)) {
                char *at = format_event_time(retry); char *deadline = xstrfmt("Retry after %s", at);
                doc_text(doc, 0, w, deadline, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(deadline); free(at);
            }
            doc_space(doc, px(8));
        }
    }
    free(s->rows); s->rows = global_pulls_active_rows(s, true, &s->row_count);
    size_t total; free(global_pulls_active_rows(s, false, &total));
    char *count = xstrfmt("Showing %zu of %zu loaded %s · newest updated first", s->row_count, total, global_board_title(s->tab));
    doc_text(doc, 0, w, count, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(count); doc_space(doc, px(8));
    if (s->tab != GLOBAL_TAB_REVIEW && (board_filter_is_on(global_pulls_filter(s)) || (s->tab == GLOBAL_TAB_PULLS && s->hide_stacked))) {
        ButtonSpec clear = {0, "Clear filters", BUTTON_PLAIN, GLOBAL_CLEAR, 0, true}; doc_button_row(doc, 0, w, &clear, 1); doc_space(doc, px(8));
    }
    if (s->loading) { doc_loading(doc, 0, w, "Reading project pull requests…"); doc_space(doc, px(8)); }
    size_t shown = s->row_count < s->visible_rows ? s->row_count : s->visible_rows;
    s->action_first = s->action_next; s->action_next += (intptr_t)shown;
    for (size_t i = 0; i < shown; i++) global_board_card(s, doc, s->rows[i], s->action_first + (intptr_t)i);
    if (!s->loading && !s->row_count) {
        const char *empty;
        if (global_board_incomplete(s, s->tab)) empty = "Rows could not all be loaded. Refresh to retry when the cooldown ends.";
        else if (s->tab == GLOBAL_TAB_REVIEW) {
            bool login = !str_empty(screen_pulls_github_login());
            for (size_t i = 0; i < end; i++) login |= json_str_nonempty(json_get(s->state.projects[i].board, "author")) != NULL;
            empty = login ? "Nothing is waiting on your review." : "Set your GitHub login to see the pull requests waiting on your review.";
        } else if (board_filter_is_on(global_pulls_filter(s)) || (s->tab == GLOBAL_TAB_PULLS && s->hide_stacked)) empty = "No rows match the filters.";
        else empty = s->tab == GLOBAL_TAB_ISSUES ? "No open issues." : "No open pull requests.";
        doc_text(doc, 0, w, empty, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    }
    if (shown < s->row_count) {
        ButtonSpec more = {0, "Show 100 more rows", BUTTON_BORDERED, GLOBAL_MORE_ROWS, 0, true}; doc_button_row(doc, 0, w, &more, 1);
    }
    if (s->limit < s->state.count) {
        char *note = xstrfmt("%zu of %zu projects included. Load the next 25 projects to extend the scope.", end, s->state.count);
        doc_text(doc, 0, w, note, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(note);
        ButtonSpec more = {0, "Load more projects", BUTTON_BORDERED, GLOBAL_MORE, 0, !s->loading}; doc_button_row(doc, 0, w, &more, 1);
    }
}
