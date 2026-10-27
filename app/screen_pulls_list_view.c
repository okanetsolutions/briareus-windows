#include "screen_pulls_internal.h"

/// `#proj-tabs`: 13px, `px-2 py-2`, the open one underlined in the accent.
typedef struct { char *label; bool active; } TabData;

static void tab_free(void *p);
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void doc_tabs(Doc *doc, int w, const char *const *labels, const int *ids, size_t count, int active);
static void layout_meeting(Doc *doc, int w, const char *transcript);
static void layout_pull(PullsScreen *s, Doc *doc, int w, size_t i);

static void tab_free(void *p) { TabData *d = p; free(d->label); free(d); }

static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    RECT t = *rc;
    draw_text(cv, d->label, &t, FONT_FOOTNOTE, d->active || hovered ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->active) { RECT u = { rc->left, rc->bottom - px(2), rc->right, rc->bottom }; fill_rect(cv, &u, theme.accent); }
}

/// The tabs `labels`, each opening the tab named by the same entry of `ids`.
static void doc_tabs(Doc *doc, int w, const char *const *labels, const int *ids, size_t count, int active) {
    int x = 0, h = px(34);
    for (size_t i = 0; i < count; i++) {
        int tw = px(8) * 2 + text_width(doc->cv, labels[i], FONT_FOOTNOTE) + px(2);
        TabData *d = xcalloc(1, sizeof *d); d->label = xstrdup(labels[i]); d->active = ids[i] == active;
        RECT rc = { x, doc->y, x + tw, doc->y + h };
        int it = doc_add(doc, &rc, paint_tab);
        doc_item(doc, it)->data = d; doc_item(doc, it)->free_data = tab_free; doc_item(doc, it)->action = ACT_TAB; doc_item(doc, it)->arg = ids[i]; doc_item(doc, it)->hand = true;
        x += tw + px(4);
    }
    doc->y += h;
    doc_rule(doc, -px(18), w + px(36));
}

/// The meeting as a chat: what the meeting said on the left, the assistant's answers on the right, and each lookup it
/// made between them.
static void layout_meeting(Doc *doc, int w, const char *transcript) {
    if (!*transcript) {
        doc_text(doc, 0, w, "The assistant is joining. What the meeting says and what it answers shows here.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    static const struct { const char *prefix, *label; } KINDS[] = { { "Meeting: ", "Meeting" }, { "Assistant: ", "Assistant" }, { "Lookup: ", NULL } };
    int bw = w * 3 / 4, pad = px(10);
    for (const char *line = transcript; *line; ) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        int kind = -1;
        for (int k = 0; k < 3 && kind < 0; k++) if (!strncmp(line, KINDS[k].prefix, strlen(KINDS[k].prefix))) kind = k;
        if (kind >= 0) {
            size_t skip = strlen(KINDS[kind].prefix);
            char *text = xstrndup(line + skip, n - skip);
            if (kind == 2) {
                char *said = xstrfmt("\xF0\x9F\x94\x8E Looked up: %s", text);
                for (char *c = said; *c; c++) if (*c == '_') *c = ' ';
                doc_text(doc, 0, w, said, FONT_CAPTION, theme.muted, DT_CENTER | DT_SINGLELINE);
                free(said);
            } else {
                bool mine = kind == 1;
                int x = mine ? w - bw : 0;
                doc_text(doc, x, bw, KINDS[kind].label, FONT_CAPTION, theme.muted, (mine ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE);
                doc_space(doc, px(2));
                int box = doc_box_begin(doc, x, bw, pad, mine ? blend(theme.accent, theme.raise, 0.18) : theme.raise, mine ? theme.accent_dim : theme.line, px(10));
                doc_text(doc, x + pad, bw - 2 * pad, text, FONT_BODY, theme.ink, DT_LEFT | DT_WORDBREAK);
                doc_box_end(doc, box, pad);
            }
            doc_space(doc, px(10));
            free(text);
        }
        line = end ? end + 1 : line + n;
    }
}

/// Pull request `i`'s row: the dashboard's buttons, the errands its state offers with the suggested one filled, then
/// Merge and its runs. Clicking the row opens the PR.
static void layout_pull(PullsScreen *s, Doc *doc, int w, size_t i) {
    const PullSummary *pull = &s->pulls[i];
    StackPosition stack; bool has_stack = stack_position_parse(json_get(pull->raw, "stack"), json_get(s->board, "stacks"), &stack);
    size_t an = 0; BoardAction *actions = str_empty(pull->branch) ? NULL : screen_pulls_row_actions(s->catalog, pull, 0, &an);
    ButtonSpec *buttons = xcalloc(an + 2, sizeof *buttons); size_t bn = 0;
    char **labels = xcalloc(an + 1, sizeof *labels);
    for (size_t k = 0; k < an && k < ACTION_STRIDE; k++) {
        bool starting = s->busy && s->starting_number == pull->number && str_eq(s->starting_id, actions[k].id);
        bool suggested = str_eq(pull->recommended, actions[k].id);
        labels[k] = starting ? xstrdup("Starting\xE2\x80\xA6") : xstrfmt("%s %s", screen_pulls_action_icon(actions[k].id), actions[k].label);
        ButtonSpec b = { 0, labels[k], suggested ? BUTTON_PROMINENT : BUTTON_BORDERED, ACT_PULL_ACTION, (intptr_t)(i * ACTION_STRIDE + k), !s->busy && !s->uncertain };
        buttons[bn++] = b;
    }
    if (!pull->draft && store_can_manage() && store_supports("pull") && store_supports("merge_pull")) {
        bool merging = s->merging_number == pull->number;
        ButtonSpec b = { 0, merging ? "Merging\xE2\x80\xA6" : "\xE2\x86\xB3 Merge", BUTTON_BORDERED, ACT_MERGE_PULL, (intptr_t)i, !s->merging_number && !s->busy };
        buttons[bn++] = b;
    }
    size_t runs = screen_pulls_runs_on(s, pull->number);
    char *runs_text = runs ? xstrfmt("%zu run%s \xE2\x80\xBA", runs, runs == 1 ? "" : "s") : NULL;
    if (runs_text) { ButtonSpec b = { 0, runs_text, BUTTON_PLAIN, ACT_RUNS, (intptr_t)i, true }; buttons[bn++] = b; }
    const char **statuses = xcalloc(pull->issue_count ? pull->issue_count : 1, sizeof *statuses);
    for (size_t k = 0; k < pull->issue_count; k++) {
        if (board_link_is_foreign(&pull->issues[k], s->project.repo)) continue;
        char *nk = screen_pulls_number_key(pull->issues[k].number); statuses[k] = json_str(json_get(s->issue_status, nk)); free(nk);
    }
    doc_pull_row(doc, 0, w, pull, has_stack ? &stack : NULL, s->project.repo, statuses, ACT_OPEN_PULL, (intptr_t)i, buttons, bn, screen_pulls_run_active_on(s, pull->number));
    free(statuses); free(runs_text); str_array_free(labels, an); free(buttons); board_actions_free(actions, an);
    if (has_stack) stack_position_free(&stack);
    doc_space(doc, px(8));
}

/// Whose review the Review List waits on: the saved GitHub login, else the project's author.
const char *screen_pulls_reviewer(PullsScreen *s) {
    const char *me = screen_pulls_github_login();
    return me ? me : json_str_nonempty(json_get(s->board, "author"));
}

/// The Review List tab's rows, as indices into the pull requests (pulls_review_list).
size_t *screen_pulls_review_rows(PullsScreen *s, size_t *count) {
    return pulls_review_list(s->pulls, s->pull_count, json_get(s->board, "stacks"), screen_pulls_reviewer(s), count);
}

void screen_pulls_pulls_layout(Screen *base, Doc *doc) {
    PullsScreen *s = (PullsScreen *)base;
    int w = doc->width;
    // SSH sessions only for a token that may read the servers; its count is the sessions open on them.
    size_t open = project_ssh_session_count(s->project.repo), files = project_sftp_session_count(s->project.repo);
    char *ssh_label = open ? xstrfmt("\xE2\x9D\xAF SSH sessions %zu", open) : xstrdup("\xE2\x9D\xAF SSH sessions");
    char *sftp_label = files ? xstrfmt("\xE2\x87\xB5 SFTP sessions %zu", files) : xstrdup("\xE2\x87\xB5 SFTP sessions");
    // Run, on the default branch, for a token that may serve one.
    const char *tabs[12] = { "\xE2\x87\x85 Pull requests", "\xE2\x8A\x99 Issues" };
    int ids[12] = { TAB_PULLS, TAB_ISSUES };
    size_t n = 2;
    // Board, after Issues, for a project that names a GitHub Projects board.
    if (board_tab_offered(&s->project)) { tabs[n] = "\xE2\x96\xA6 Board"; ids[n++] = TAB_BOARD; }
    else if (s->tab == TAB_BOARD) s->tab = TAB_PULLS;
    // Review List, the pull requests waiting on the user's review, after the board.
    size_t review_count; free(screen_pulls_review_rows(s, &review_count));
    char *review_label = s->loaded ? xstrfmt("\xE2\x9C\x93 Review List %zu", review_count) : xstrdup("\xE2\x9C\x93 Review List");
    tabs[n] = review_label; ids[n++] = TAB_REVIEW;
    // Files, the repository's tree, for a server that reads it.
    if (project_files_offered()) { tabs[n] = "\xF0\x9F\x97\x82 Files"; ids[n++] = TAB_FILES; }
    else if (s->tab == TAB_FILES) s->tab = TAB_PULLS;
    if (project_run_offered()) { tabs[n] = "\xE2\x96\xB6 Run"; ids[n++] = TAB_RUN; }
    if (project_ssh_offered()) { tabs[n] = ssh_label; ids[n++] = TAB_SSH; tabs[n] = sftp_label; ids[n++] = TAB_SFTP; }
    if (project_db_offered()) { tabs[n] = "\xE2\x9B\x81 Database"; ids[n++] = TAB_DB; }
    if (project_forge_offered()) { tabs[n] = "\xE2\x98\x81 Forge"; ids[n++] = TAB_FORGE; }
    // The meeting's transcript, while one runs on this project and after it.
    char *transcript = meeting_transcript_for(s->project.repo);
    if (transcript) { tabs[n] = meeting_for(s->project.repo) ? "\xF0\x9F\x8E\x99 Meeting \xE2\x97\x8F" : "\xF0\x9F\x8E\x99 Meeting"; ids[n++] = TAB_MEETING; }
    else if (s->tab == TAB_MEETING) s->tab = TAB_PULLS;
    doc_tabs(doc, w, tabs, ids, n, s->tab);
    free(ssh_label); free(sftp_label); free(review_label);
    doc_space(doc, px(14));
    if (s->tab == TAB_BOARD) { board_tab_layout(s->board_tab, doc, w); free(transcript); return; }
    if (s->tab == TAB_FILES) { project_files_layout(s->files, doc, w); free(transcript); return; }
    if (s->tab == TAB_RUN) { project_run_layout(s->run, doc, w); return; }
    if (s->tab == TAB_SSH) { project_ssh_layout(s->ssh, doc, w); return; }
    if (s->tab == TAB_SFTP) { project_sftp_layout(s->sftp, doc, w); return; }
    if (s->tab == TAB_DB) { project_db_layout(s->db, doc, w); return; }
    if (s->tab == TAB_FORGE) { project_forge_layout(s->forge, doc, w); free(transcript); return; }
    if (s->tab == TAB_MEETING) { layout_meeting(doc, w, transcript); free(transcript); return; }
    free(transcript);
    if (s->error) { doc_notice(doc, 0, w, s->error); doc_space(doc, px(10)); }
    if (s->write_error) {
        doc_notice(doc, 0, w, s->write_error);
        if (s->uncertain) { doc_space(doc, px(4)); doc_text(doc, 0, w, "The request may have completed. Refresh (F5) and look for its conversation in the project before starting another agent.", FONT_CAPTION, theme.muted, DT_WORDBREAK); }
        doc_space(doc, px(10));
    }
    if (s->tab == TAB_REVIEW) {
        size_t rn; size_t *rows = screen_pulls_review_rows(s, &rn);
        for (size_t k = 0; k < rn; k++) layout_pull(s, doc, w, rows[k]);
        free(rows);
        if (s->loaded && !rn && !s->error) doc_text(doc, 0, w, screen_pulls_reviewer(s) ? "Nothing is waiting on your review." : "Set your GitHub login to see the pull requests waiting on your review.", FONT_FOOTNOTE, theme.muted, DT_LEFT);
        if (!s->loaded) doc_loading(doc, 0, w, "Loading pull requests\xE2\x80\xA6");
        doc_space(doc, px(16));
        return;
    }
    BoardFilter *filter = screen_pulls_current_filter(s);
    size_t total = s->tab == 0 ? s->pull_count : s->issue_count, shown = 0;
    size_t rn; BoardRow *rows = screen_pulls_rows_of(s, &rn);
    for (size_t i = 0; i < rn; i++) if (screen_pulls_row_filter_passes(s, &rows[i], i)) shown++;
    if (board_filter_is_on(filter) || (s->tab == TAB_PULLS && s->hide_later_stacked)) {
        char *text = xstrfmt("Showing %zu of %zu", shown, total);
        int y = doc->y;
        int cw = text_width(doc->cv, "Clear filters", FONT_CAPTION) + px(8);
        RECT tr = { 0, y, w - cw - px(8), y + px(20) };
        doc_text_at(doc, &tr, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT cr = { w - cw, y, w, tr.bottom };
        int ci = doc_text_at(doc, &cr, "Clear filters", FONT_CAPTION, theme.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        doc_item(doc, ci)->action = ACT_CLEAR; doc_item(doc, ci)->hand = true;
        doc->y = tr.bottom + px(8);
        free(text);
    }
    if (s->tab == 0) {
        for (size_t i = 0; i < s->pull_count; i++) {
            if (!screen_pulls_row_filter_passes(s, &rows[i], i)) continue;
            layout_pull(s, doc, w, i);
        }
        if (s->loaded && !shown && !s->error) doc_text(doc, 0, w, s->pull_count ? "No pull requests match the filters." : "No open pull requests.", FONT_FOOTNOTE, theme.muted, DT_LEFT);
    } else {
        const char *refused = json_str(json_get(s->board, "issuesError"));
        if (refused) {
            int box = doc_box_begin(doc, 0, w, px(10), theme.raise, theme.line, px(12));
            doc_item(doc, box)->hover_fill = false;
            doc_text(doc, px(12), w - px(24), "GitHub would not read this repository\xE2\x80\x99s issues with the server\xE2\x80\x99s token. A fine-grained token needs Issues: read. The pull requests are unaffected.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
            doc_space(doc, px(6));
            doc_notice(doc, px(12), w - px(24), refused);
            doc_box_end(doc, box, px(10));
        } else {
            IssueSummary *visible = xcalloc(s->issue_count, sizeof *visible);
            size_t *map = xcalloc(s->issue_count, sizeof *map);
            size_t vn = 0;
            for (size_t i = 0; i < s->issue_count; i++) if (board_filter_passes(filter, &rows[i], -1)) { visible[vn] = s->issues[i]; map[vn] = i; vn++; }
            size_t nn; IssueRow *nested = issues_nested(visible, vn, s->project.repo, &nn);
            nn = issues_folded(nested, nn, visible, &s->issue_folds);
            for (size_t k = 0; k < nn; k++) {
                int depth = nested[k].depth > 4 ? 4 : nested[k].depth;
                int indent = depth * px(18);
                doc_issue_row_foldable(doc, indent, w - indent, &s->issues[map[nested[k].index]], s->project.repo, depth > 0, ACT_OPEN_ISSUE, (intptr_t)map[nested[k].index],
                    nested[k].children ? ACT_FOLD_ISSUE : 0, issue_folds_contains(&s->issue_folds, visible[nested[k].index].number));
                doc_space(doc, px(8));
            }
            free(nested); free(map); free(visible);
            if (json_bool_is(json_get(s->board, "issuesTruncated"), true)) doc_text(doc, 0, w, "This repository has more open issues; only the most recently updated are listed.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
            if (s->loaded && !shown && !s->error) doc_text(doc, 0, w, s->issue_count ? "No issues match the filters." : "No open issues.", FONT_FOOTNOTE, theme.muted, DT_LEFT);
        }
    }
    free(rows);
    if (!s->loaded) doc_loading(doc, 0, w, "Loading pull requests\xE2\x80\xA6");
    doc_space(doc, px(16));
}
