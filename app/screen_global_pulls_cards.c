#include "screen_global_pulls_internal.h"

const char *global_board_title(GlobalBoardTab tab) {
    return tab == GLOBAL_TAB_ISSUES ? "issues" : tab == GLOBAL_TAB_REVIEW ? "Review List" : "pull requests";
}
BoardFilter *global_pulls_filter(GlobalPullsScreen *s) { return s->tab == GLOBAL_TAB_ISSUES ? &s->issue_filter : &s->filter; }
GlobalPullRow *global_pulls_active_rows(GlobalPullsScreen *s, bool filtered, size_t *count) {
    return global_board_rows(&s->state, s->limit, s->tab, filtered ? global_pulls_filter(s) : NULL,
        filtered && s->hide_stacked, screen_pulls_github_login(), count);
}
void global_board_tabs(GlobalPullsScreen *s, Doc *doc) {
    char *labels[GLOBAL_TAB_COUNT]; ButtonSpec tabs[GLOBAL_TAB_COUNT];
    for (int tab = 0; tab < GLOBAL_TAB_COUNT; tab++) {
        size_t count; free(global_board_rows(&s->state, s->limit, (GlobalBoardTab)tab, NULL, false, screen_pulls_github_login(), &count));
        labels[tab] = xstrfmt("%s %zu%s", tab == GLOBAL_TAB_PULLS ? "Pull requests" : tab == GLOBAL_TAB_ISSUES ? "Issues" : "Review List", count, global_board_incomplete(s, (GlobalBoardTab)tab) ? "+" : ""); tabs[tab] = (ButtonSpec){0, labels[tab], s->tab == (GlobalBoardTab)tab ? BUTTON_PROMINENT : BUTTON_BORDERED, GLOBAL_TAB, tab, true};
    }
    doc_button_row(doc, 0, doc->width, tabs, GLOBAL_TAB_COUNT);
    for (int tab = 0; tab < GLOBAL_TAB_COUNT; tab++) free(labels[tab]);
    doc_space(doc, px(10));
}
void global_board_card(GlobalPullsScreen *s, Doc *doc, GlobalPullRow r, intptr_t action) {
    int w = doc->width; GlobalPullProject *p = &s->state.projects[r.project];
    char *ago = p->synced_at ? format_relative(p->synced_at) : xstrdup("unknown");
    char *context = xstrfmt("%s · %s · synced %s", project_title(&p->project), p->project.repo, ago);
    doc_text(doc, 0, w, context, FONT_CAPTION, theme.accent, DT_WORDBREAK); free(context); free(ago); doc_space(doc, px(3));
    int box = (int)doc->count, number; GlobalBoardTab kind;
    if (s->tab == GLOBAL_TAB_ISSUES) {
        IssueSummary *issue = &p->issues[r.pull]; number = issue->number; kind = GLOBAL_TAB_ISSUES;
        doc_issue_row(doc, 0, w, issue, p->project.repo, false, store_supports("issue") ? GLOBAL_OPEN : 0, action);
    } else {
        PullSummary *pull = &p->pulls[r.pull]; number = pull->number; kind = GLOBAL_TAB_PULLS;
        StackPosition stack; bool stacked = stack_position_parse(json_get(pull->raw, "stack"), json_get(p->board, "stacks"), &stack);
        doc_pull_row(doc, 0, w, pull, stacked ? &stack : NULL, p->project.repo, NULL, store_supports("pull") ? GLOBAL_OPEN : 0, action, NULL, 0, false);
        if (stacked) stack_position_free(&stack);
    }
    if (s->selected_kind == kind && str_ieq(s->selected_repo, p->project.repo) && s->selected_number == number) doc_item(doc, box)->border = theme.accent;
    doc_space(doc, px(10));
}
