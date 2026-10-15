#include "screen_pulls_internal.h"

static Json *filter_json(const BoardFilter *f);
static void filter_read(BoardFilter *f, const Json *j);

BoardFilter *screen_pulls_current_filter(PullsScreen *s) { return s->tab == 0 ? &s->pull_filter : &s->issue_filter; }

bool screen_pulls_stack_filter_passes(const PullsScreen *s, size_t index) {
    if (!s->hide_later_stacked) return true;
    StackPosition stack;
    if (!stack_position_parse(json_get(s->pulls[index].raw, "stack"), NULL, &stack)) return true;
    bool first = stack.position <= 1;
    stack_position_free(&stack);
    return first;
}

bool screen_pulls_row_filter_passes(PullsScreen *s, const BoardRow *row, size_t index) {
    return board_filter_passes(screen_pulls_current_filter(s), row, -1) && (s->tab != TAB_PULLS || screen_pulls_stack_filter_passes(s, index));
}

BoardRow *screen_pulls_rows_of(PullsScreen *s, size_t *count) {
    size_t n = s->tab == 0 ? s->pull_count : s->issue_count;
    BoardRow *rows = xcalloc(n, sizeof *rows);
    for (size_t i = 0; i < n; i++) rows[i] = s->tab == 0 ? pull_board_row(&s->pulls[i]) : issue_board_row(&s->issues[i]);
    *count = n;
    return rows;
}

// The pickers are kept on disk per repository, so coming back to the board shows what was picked last.
static Json *filter_json(const BoardFilter *f) {
    Json *j = json_object();
    for (int k = 0; k < FILTER_KIND_COUNT; k++) { const char *v = board_filter_get(f, (FilterKind)k); if (!str_empty(v)) json_set_str(j, filter_kind_name((FilterKind)k), v); }
    return j;
}

static void filter_read(BoardFilter *f, const Json *j) {
    for (int k = 0; k < FILTER_KIND_COUNT; k++) { const char *v = json_str(json_get(j, filter_kind_name((FilterKind)k))); if (v) board_filter_set(f, (FilterKind)k, v); }
}

void screen_pulls_filters_save(PullsScreen *s) {
    Json *saved = json_object();
    Json *pulls = filter_json(&s->pull_filter);
    json_set_bool(pulls, "hideLaterStacked", s->hide_later_stacked);
    json_object_set(saved, "pulls", pulls);
    json_object_set(saved, "issues", filter_json(&s->issue_filter));
    char *key = xstrfmt("pulls-filters:%s", s->project.repo);
    cache_store(g_store.cache, saved, key);
    free(key); json_free(saved);
}

/// A board never filtered opens on the project's author; one with saved picks opens on them.
bool screen_pulls_filters_restore(PullsScreen *s) {
    char *key = xstrfmt("pulls-filters:%s", s->project.repo);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    if (!saved) return false;
    filter_read(&s->pull_filter, json_get(saved, "pulls"));
    s->hide_later_stacked = json_bool_is(json_get(json_get(saved, "pulls"), "hideLaterStacked"), true);
    filter_read(&s->issue_filter, json_get(saved, "issues"));
    json_free(saved);
    return true;
}

char *screen_pulls_number_key(int number) { return xstrfmt("%d", number); }

void screen_pulls_pulls_header(Screen *base, HeaderInfo *info) {
    PullsScreen *s = (PullsScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", project_title(&s->project));
    Str sub; str_init(&sub);
    str_appendz(&sub, s->project.repo);
    if (s->loaded && s->tab == TAB_REVIEW) { size_t n; free(screen_pulls_review_rows(s, &n)); str_appendf(&sub, " \xC2\xB7 %zu waiting on %s", n, screen_pulls_reviewer(s) ? screen_pulls_reviewer(s) : "you"); }
    else if (s->loaded) str_appendf(&sub, " \xC2\xB7 %zu open pull request%s", s->pull_count, s->pull_count == 1 ? "" : "s");
    if (s->synced_at) { char *ago = format_relative(s->synced_at); str_appendf(&sub, " \xC2\xB7 synced %s", ago); free(ago); }
    time_t retry = pulls_retry_deadline(s->project.repo);
    if (retry > time(NULL)) { char *at = format_event_time(retry); str_appendf(&sub, " \xC2\xB7 retry after %s", at); free(at); }
    // A meeting about this project leads the line: its time, cost and what it is doing.
    if (meeting_for(s->project.repo)) { char *m = meeting_status(); snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s", m, sub.data); free(m); }
    else snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
    str_free(&sub);
    // 🎙 Meet: the meeting assistant, on this project.
    {
        bool here = meeting_for(s->project.repo);
        HeaderButton *m = &info->buttons[info->button_count++];
        snprintf(m->label, sizeof m->label, "%s", here ? "\xF0\x9F\x8E\x99 Meeting \xE2\x97\x8F" : "\xF0\x9F\x8E\x99 Meet");
        m->glyph = 0xE720; m->action = ACT_MEET; m->enabled = true; m->tip = "Join a meeting with an assistant that can look up this project";
    }
    if (s->tab == TAB_MEETING) return;
    if (s->tab == TAB_RUN) { project_run_header(s->run, info); return; }
    if (s->tab == TAB_BOARD) {
        board_tab_header(s->board_tab, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the board from GitHub again";
        return;
    }
    if (s->tab == TAB_FILES) {
        project_files_header(s->files, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the tree and the open files again";
        return;
    }
    if (s->tab == TAB_SSH) {
        project_ssh_header(s->ssh, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the project's SSH servers again";
        return;
    }
    if (s->tab == TAB_DB) {
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the project's SSH servers again";
        return;
    }
    if (s->tab == TAB_FORGE) {
        project_forge_header(s->forge, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read it from Forge again";
        return;
    }
    if (s->tab == TAB_REVIEW) {
        // Whose review the list waits on, set or changed here as from "Assign me".
        HeaderButton *l = &info->buttons[info->button_count++];
        if (screen_pulls_github_login()) snprintf(l->label, sizeof l->label, "Reviewing as %s", screen_pulls_github_login()); else snprintf(l->label, sizeof l->label, "Set my GitHub login\xE2\x80\xA6");
        l->glyph = 0xE77B; l->action = ACT_REVIEW_LOGIN; l->enabled = true; l->tip = "The GitHub login whose reviews the Review List shows";
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = !s->req && store_supports("pulls") && time(NULL) >= pulls_retry_deadline(s->project.repo); r->tip = "Refresh the list (the server may return cached data)";
        return;
    }
    if (s->tab == TAB_SFTP) {
        project_sftp_header(s->sftp, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the servers and the folder on show again";
        return;
    }
    // The pickers, as the dashboard's selects, and ⟳.
    {
        BoardFilter *f = screen_pulls_current_filter(s);
        const char *author = board_filter_get(f, FILTER_AUTHOR), *screen_pulls_reviewer = board_filter_get(f, FILTER_REVIEWER), *assignee = board_filter_get(f, FILTER_ASSIGNEE), *label = board_filter_get(f, FILTER_LABEL);
        HeaderButton *b;
        b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(author) ? "All authors" : author); b->action = ACT_FILTER_AUTHOR; b->enabled = s->loaded;
        b->glyph = 0xE77B; b->tip = "Filter by author";
        if (s->tab == 0) { b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(screen_pulls_reviewer) ? "All reviewers" : screen_pulls_reviewer); b->action = ACT_FILTER_REVIEWER; b->enabled = s->loaded; b->glyph = 0xE73E; b->tip = "Filter by reviewer"; }
        if (s->tab == TAB_ISSUES) { b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(assignee) ? "All assignees" : str_eq(assignee, PROJECT_NO_ASSIGNEE) ? "No assignee" : assignee); b->action = ACT_FILTER_ASSIGNEE; b->enabled = s->loaded; }
        b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(label) ? "All labels" : label); b->action = ACT_FILTER_LABEL; b->enabled = s->loaded;
        b->glyph = 0xE8EC; b->tip = "Filter by label";
        if (s->tab == TAB_PULLS) {
            b = &info->buttons[info->button_count++];
            snprintf(b->label, sizeof b->label, "%s", s->hide_later_stacked ? "Show later stacked PRs" : "Hide later stacked PRs");
            b->glyph = 0xE71C; b->action = ACT_FILTER_STACK; b->enabled = s->loaded;
            b->tip = "Hide or show stacked pull requests after position 1; keep standalone pull requests visible";
        }
    }
    HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = !s->req && store_supports("pulls") && time(NULL) >= pulls_retry_deadline(s->project.repo); r->tip = "Refresh the list (the server may return cached data)";
}

/// One picker's menu: `All <kind>s`, then each option with how many rows it would show.
void screen_pulls_filter_pick(PullsScreen *s, FilterKind kind, POINT pt) {
    BoardFilter *filter = screen_pulls_current_filter(s);
    size_t rn; BoardRow *rows = screen_pulls_rows_of(s, &rn);
    if (s->tab == TAB_PULLS && s->hide_later_stacked) {
        size_t shown = 0;
        for (size_t i = 0; i < rn; i++) if (screen_pulls_stack_filter_passes(s, i)) rows[shown++] = rows[i];
        rn = shown;
    }
    size_t count; FilterOption *options = board_filter_options(filter, kind, rows, rn, &count);
    HMENU menu = CreatePopupMenu();
    const char *pick = board_filter_get(filter, kind);
    char *all = xstrfmt("All %ss", filter_kind_name(kind)); wchar_t *wall = utf8_to_wide(all);
    AppendMenuW(menu, MF_STRING | (str_empty(pick) ? MF_CHECKED : 0), 1, wall);
    free(all); free(wall);
    for (size_t i = 0; i < count; i++) {
        char *label = xstrfmt("%s (%d)", options[i].text, options[i].count); wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (str_eq(pick, options[i].value) ? MF_CHECKED : 0), (UINT_PTR)(2 + i), wl);
        free(label); free(wl);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) board_filter_set(filter, kind, "");
    else if (chosen >= 2 && (size_t)(chosen - 2) < count) board_filter_set(filter, kind, options[chosen - 2].value);
    if (chosen > 0) { s->has_opening = false; screen_pulls_filters_save(s); }
    filter_options_free(options, count);
    free(rows);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
