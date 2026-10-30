#include "screen_global_pulls_internal.h"

void global_pulls_header(Screen *base, HeaderInfo *info) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    snprintf(info->title, sizeof info->title, "All projects · %s", global_board_title(s->tab));
    if (!s->listed) snprintf(info->subtitle, sizeof info->subtitle, "%s", s->loading ? "Reading accessible project scope" : "Accessible project scope unavailable");
    else snprintf(info->subtitle, sizeof info->subtitle, "%zu accessible projects · %s", s->state.count, global_board_incomplete(s, s->tab) ? "scope incomplete" : "all projects loaded");
    if (!global_pulls_current(s)) return;
    const FilterKind kinds[] = {FILTER_AUTHOR, s->tab == GLOBAL_TAB_ISSUES ? FILTER_ASSIGNEE : FILTER_REVIEWER, FILTER_LABEL};
    for (size_t i = 0; s->tab != GLOBAL_TAB_REVIEW && i < sizeof kinds / sizeof kinds[0]; i++) {
        FilterKind kind = kinds[i]; HeaderButton *b = &info->buttons[info->button_count++];
        const char *pick = board_filter_get(global_pulls_filter(s), kind);
        if (str_empty(pick)) snprintf(b->label, sizeof b->label, "All %ss ▾", filter_kind_name(kind));
        else snprintf(b->label, sizeof b->label, "%s ▾", pick);
        b->glyph = kind == FILTER_AUTHOR ? 0xE77B : kind == FILTER_REVIEWER ? 0xE73E : 0xE8EC; b->tip = kind == FILTER_AUTHOR ? "Filter by author" : kind == FILTER_REVIEWER ? "Filter by reviewer" : kind == FILTER_ASSIGNEE ? "Filter by assignee" : "Filter by label";
        b->action = GLOBAL_FILTER_BASE + kind; b->enabled = s->listed;
    }
    HeaderButton *b;
    if (s->tab == GLOBAL_TAB_PULLS) {
    b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s later stacked PRs", s->hide_stacked ? "Show" : "Hide"); b->glyph = 0xE71C; b->tip = "Show or hide later stacked pull requests"; b->action = GLOBAL_STACK; b->enabled = s->listed;
    b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s failing checks", s->filter.hide_failing_checks ? "Show" : "Hide"); b->glyph = 0xEA39; b->tip = "Show or hide pull requests with failing checks"; b->action = GLOBAL_CHECKS; b->enabled = s->listed;
    }
    if (s->tab == GLOBAL_TAB_REVIEW) {
        b = &info->buttons[info->button_count++]; b->glyph = 0xE77B; b->action = GLOBAL_LOGIN; b->enabled = true;
        const char *login = screen_pulls_github_login();
        snprintf(b->label, sizeof b->label, "%s", login ? login : "Set my GitHub login"); b->tip = "Whose review the list waits on; otherwise each project's author";
    }
    b = &info->buttons[info->button_count++]; b->glyph = 0xE72C; b->action = GLOBAL_REFRESH;
    time_t shared = global_pulls_projects_deadline();
    if (shared > s->projects_retry) s->projects_retry = shared;
    b->enabled = time(NULL) >= s->projects_retry; b->tip = "Refresh all included repositories from GitHub; repository cooldowns still apply";
}
static const char *filter_key(const GlobalPullsScreen *s) { return s->tab == GLOBAL_TAB_ISSUES ? "global-issues-filters" : "global-pulls-filters"; }
void global_pulls_filters_save(GlobalPullsScreen *s) {
    if (s->tab == GLOBAL_TAB_REVIEW) return;
    const BoardFilter *filter = global_pulls_filter(s); Json *saved = json_object();
    for (int k = 0; k < FILTER_KIND_COUNT; k++) json_set_str(saved, filter_kind_name((FilterKind)k), board_filter_get(filter, (FilterKind)k));
    json_set_bool(saved, "hideFailingChecks", filter->hide_failing_checks); json_set_bool(saved, "hideLaterStacked", s->hide_stacked);
    cache_store(g_store.cache, saved, filter_key(s)); json_free(saved);
}
void global_board_filters_restore(GlobalPullsScreen *s) {
    for (int tab = GLOBAL_TAB_PULLS; tab <= GLOBAL_TAB_ISSUES; tab++) {
        s->tab = (GlobalBoardTab)tab; BoardFilter *filter = global_pulls_filter(s);
        Json *saved = cache_value(g_store.cache, filter_key(s));
        for (int k = 0; k < FILTER_KIND_COUNT; k++) {
            if (s->tab == GLOBAL_TAB_ISSUES && k == FILTER_REVIEWER) continue;
            board_filter_set(filter, (FilterKind)k, json_str(json_get(saved, filter_kind_name((FilterKind)k))));
        }
        if (s->tab == GLOBAL_TAB_PULLS) {
            filter->hide_failing_checks = json_bool_is(json_get(saved, "hideFailingChecks"), true);
            s->hide_stacked = json_bool_is(json_get(saved, "hideLaterStacked"), true);
        }
        json_free(saved);
    }
    s->tab = GLOBAL_TAB_PULLS;
}
void global_pulls_pick(GlobalPullsScreen *s, FilterKind kind, POINT pt) {
    if (kind < 0 || kind >= FILTER_KIND_COUNT || s->tab == GLOBAL_TAB_REVIEW) return;
    if (s->tab == GLOBAL_TAB_ISSUES && kind == FILTER_REVIEWER) return;
    size_t n; GlobalPullRow *refs = global_board_rows(&s->state, s->limit, s->tab, NULL, s->hide_stacked, NULL, &n);
    BoardRow *rows = xcalloc(n, sizeof *rows);
    for (size_t i = 0; i < n; i++) rows[i] = global_board_row(&s->state, refs[i], s->tab);
    size_t count; FilterOption *options = board_filter_options(global_pulls_filter(s), kind, rows, n, &count);
    HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"All");
    for (size_t i = 0; i < count; i++) {
        char *text = xstrfmt("%s (%d)", options[i].text, options[i].count); wchar_t *wide = utf8_to_wide(text);
        AppendMenuW(menu, MF_STRING, (UINT_PTR)i + 2, wide); free(text); free(wide);
    }
    GlobalBoardTab tab = s->tab;
    Pane *source = s->base.pane; Screen *screen = &s->base; HWND window = pane_hwnd(source);
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, 0, window, NULL); DestroyMenu(menu);
    // The popup pumps messages: connection rebuilding can destroy its screen before it returns.
    bool live = IsWindow(window) && pane_contains_screen(source, screen);
    if (!live) { filter_options_free(options, count); free(rows); free(refs); return; }
    if (pane_top(source) != screen || s->tab != tab || !global_pulls_current(s)) { filter_options_free(options, count); free(rows); free(refs); return; }
    if (choice == 1) board_filter_set(global_pulls_filter(s), kind, "");
    else if (choice >= 2 && (size_t)(choice - 2) < count) board_filter_set(global_pulls_filter(s), kind, options[choice - 2].value);
    filter_options_free(options, count); free(rows); free(refs); global_pulls_filters_save(s);
    global_pulls_changed(s);
}
