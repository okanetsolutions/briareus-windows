#include "screen_global_pulls_internal.h"

static bool incomplete(const GlobalPullsScreen *s) {
    if (!s->listed || s->loading || s->error || s->limit < s->state.count) return true;
    for (size_t i = 0; i < s->state.count; i++) {
        const GlobalPullProject *p = &s->state.projects[i];
        if (!p->loaded || p->error || p->truncated) return true;
    }
    return false;
}
void global_pulls_layout(Screen *base, Doc *doc) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base; int w = doc->width;
    if (!global_pulls_current(s)) { doc_notice(doc, 0, w, "Reopen All projects with access to project pull requests."); return; }
    if (s->error) { doc_notice(doc, 0, w, s->error); doc_space(doc, px(8)); }
    if (incomplete(s)) {
        doc_notice(doc, 0, w, "Scope incomplete. Counts cover loaded snapshots only; unavailable or unread projects may have more pull requests."); doc_space(doc, px(10));
    }
    size_t end = s->state.count < s->limit ? s->state.count : s->limit;
    for (size_t i = 0; i < end; i++) {
        GlobalPullProject *p = &s->state.projects[i];
        if (p->error || p->truncated) {
            char *note = xstrfmt("%s: %s", p->project.repo, p->error ? p->error : "Only the first 500 pull requests are included; open this project for the full list.");
            doc_notice(doc, 0, w, note); free(note);
            time_t retry = pulls_retry_deadline(p->project.repo);
            if (retry > time(NULL)) {
                char *at = format_event_time(retry); char *deadline = xstrfmt("Retry after %s", at);
                doc_text(doc, 0, w, deadline, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(deadline); free(at);
            }
            doc_space(doc, px(8));
        }
    }
    free(s->rows); s->rows = global_pulls_rows(&s->state, s->limit, &s->filter, s->hide_stacked, &s->row_count);
    size_t total; free(global_pulls_rows(&s->state, s->limit, NULL, false, &total));
    char *count = xstrfmt("Showing %zu of %zu loaded pull requests · newest updated first", s->row_count, total);
    doc_text(doc, 0, w, count, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(count); doc_space(doc, px(8));
    if (board_filter_is_on(&s->filter) || s->hide_stacked) {
        ButtonSpec clear = {0, "Clear filters", BUTTON_PLAIN, GLOBAL_CLEAR, 0, true}; doc_button_row(doc, 0, w, &clear, 1); doc_space(doc, px(8));
    }
    if (s->loading) { doc_loading(doc, 0, w, "Reading project pull requests…"); doc_space(doc, px(8)); }
    size_t shown = s->row_count < s->visible_rows ? s->row_count : s->visible_rows;
    s->action_first = s->action_next; s->action_next += (intptr_t)shown;
    for (size_t i = 0; i < shown; i++) {
        GlobalPullRow r = s->rows[i]; GlobalPullProject *p = &s->state.projects[r.project]; PullSummary *pull = &p->pulls[r.pull];
        char *ago = p->synced_at ? format_relative(p->synced_at) : xstrdup("unknown");
        char *context = xstrfmt("%s · %s · synced %s", project_title(&p->project), p->project.repo, ago);
        doc_text(doc, 0, w, context, FONT_CAPTION, theme.accent, DT_WORDBREAK); free(context); free(ago); doc_space(doc, px(3));
        StackPosition stack; bool stacked = stack_position_parse(json_get(pull->raw, "stack"), json_get(p->board, "stacks"), &stack);
        int box = (int)doc->count;
        doc_pull_row(doc, 0, w, pull, stacked ? &stack : NULL, p->project.repo, NULL, GLOBAL_OPEN, s->action_first + (intptr_t)i, NULL, 0, false);
        if (str_ieq(s->selected_repo, p->project.repo) && s->selected_number == pull->number) doc_item(doc, box)->border = theme.accent;
        if (stacked) stack_position_free(&stack);
        doc_space(doc, px(10));
    }
    if (!s->loading && !s->row_count) {
        const char *empty = incomplete(s) ? "Pull requests could not all be loaded. Refresh to retry when the cooldown ends." :
            board_filter_is_on(&s->filter) || s->hide_stacked ? "No pull requests match the filters." : "No open pull requests.";
        doc_text(doc, 0, w, empty, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    }
    if (shown < s->row_count) {
        ButtonSpec more = {0, "Show 100 more pull requests", BUTTON_BORDERED, GLOBAL_MORE_ROWS, 0, true}; doc_button_row(doc, 0, w, &more, 1);
    }
    if (s->limit < s->state.count) {
        char *note = xstrfmt("%zu of %zu projects included. Load the next 25 projects to extend the scope.", end, s->state.count);
        doc_text(doc, 0, w, note, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(note);
        ButtonSpec more = {0, "Load more projects", BUTTON_BORDERED, GLOBAL_MORE, 0, !s->loading}; doc_button_row(doc, 0, w, &more, 1);
    }
}
void global_pulls_header(Screen *base, HeaderInfo *info) {
    GlobalPullsScreen *s = (GlobalPullsScreen *)base;
    snprintf(info->title, sizeof info->title, "All projects · Pull requests");
    if (!s->listed) snprintf(info->subtitle, sizeof info->subtitle, "%s", s->loading ? "Reading accessible project scope" : "Accessible project scope unavailable");
    else snprintf(info->subtitle, sizeof info->subtitle, "%zu accessible projects · %s", s->state.count, incomplete(s) ? "scope incomplete" : "all projects loaded");
    if (!global_pulls_current(s)) return;
    const FilterKind kinds[] = {FILTER_AUTHOR, FILTER_REVIEWER, FILTER_LABEL};
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        FilterKind kind = kinds[i]; HeaderButton *b = &info->buttons[info->button_count++];
        const char *pick = board_filter_get(&s->filter, kind);
        if (str_empty(pick)) snprintf(b->label, sizeof b->label, "All %ss ▾", filter_kind_name(kind));
        else snprintf(b->label, sizeof b->label, "%s ▾", pick);
        b->glyph = kind == FILTER_AUTHOR ? 0xE77B : kind == FILTER_REVIEWER ? 0xE73E : 0xE8EC; b->tip = kind == FILTER_AUTHOR ? "Filter by author" : kind == FILTER_REVIEWER ? "Filter by reviewer" : "Filter by label";
        b->action = GLOBAL_FILTER_BASE + kind; b->enabled = s->listed;
    }
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s later stacked PRs", s->hide_stacked ? "Show" : "Hide"); b->glyph = 0xE71C; b->tip = "Show or hide later stacked pull requests"; b->action = GLOBAL_STACK; b->enabled = s->listed;
    b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s failing checks", s->filter.hide_failing_checks ? "Show" : "Hide"); b->glyph = 0xEA39; b->tip = "Show or hide pull requests with failing checks"; b->action = GLOBAL_CHECKS; b->enabled = s->listed;
    b = &info->buttons[info->button_count++]; b->glyph = 0xE72C; b->action = GLOBAL_REFRESH;
    b->enabled = time(NULL) >= s->projects_retry; b->tip = "Refresh all included repositories from GitHub; repository cooldowns still apply";
}
void global_pulls_filters_save(GlobalPullsScreen *s) {
    Json *saved = json_object();
    for (int k = 0; k < FILTER_KIND_COUNT; k++) json_set_str(saved, filter_kind_name((FilterKind)k), board_filter_get(&s->filter, (FilterKind)k));
    json_set_bool(saved, "hideFailingChecks", s->filter.hide_failing_checks); json_set_bool(saved, "hideLaterStacked", s->hide_stacked);
    cache_store(g_store.cache, saved, "global-pulls-filters"); json_free(saved);
}
void global_pulls_pick(GlobalPullsScreen *s, FilterKind kind, POINT pt) {
    if (kind < 0 || kind >= FILTER_KIND_COUNT) return;
    size_t n; GlobalPullRow *refs = global_pulls_rows(&s->state, s->limit, NULL, s->hide_stacked, &n);
    BoardRow *rows = xcalloc(n, sizeof *rows);
    for (size_t i = 0; i < n; i++) rows[i] = pull_board_row(&s->state.projects[refs[i].project].pulls[refs[i].pull]);
    size_t count; FilterOption *options = board_filter_options(&s->filter, kind, rows, n, &count);
    HMENU menu = CreatePopupMenu(); AppendMenuW(menu, MF_STRING, 1, L"All");
    for (size_t i = 0; i < count; i++) {
        char *text = xstrfmt("%s (%d)", options[i].text, options[i].count); wchar_t *wide = utf8_to_wide(text);
        AppendMenuW(menu, MF_STRING, (UINT_PTR)i + 2, wide); free(text); free(wide);
    }
    int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL); DestroyMenu(menu);
    if (!global_pulls_current(s)) { filter_options_free(options, count); free(rows); free(refs); return; }
    if (choice == 1) board_filter_set(&s->filter, kind, "");
    else if (choice >= 2 && (size_t)(choice - 2) < count) board_filter_set(&s->filter, kind, options[choice - 2].value);
    filter_options_free(options, count); free(rows); free(refs); global_pulls_filters_save(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
