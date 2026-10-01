// The project board: open pull requests and issues, one pull request in full, and one issue.
#include "dialogs.h"
#include "screens.h"
#include "str.h"
#include "webview.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// MARK: - Errands, shared by the board and one pull request

static const char *action_icon(const char *id) {
    static const struct { const char *id; const char *icon; } icons[] = {
        { "run", "\xE2\x96\xB6" }, { "review", "\xE2\x8C\x95" }, { "solve-conflicts", "\xF0\x9F\x94\x80" }, { "fix-checks", "\xF0\x9F\xA7\xAA" }, { "implement-feedback", "\xF0\x9F\x92\xAC" },
        { "custom-feedback", "\xE2\x9C\x8D" }, { "pr-body-summary", "\xE2\x9C\x8E" }, { "delete-self-comments", "\xF0\x9F\xA7\xB9" },
    };
    for (size_t k = 0; k < sizeof icons / sizeof *icons; k++) if (str_eq(icons[k].id, id)) return icons[k].icon;
    return "";
}
/// Asks for an errand's input when it takes one; the others start straight away. True to go ahead.
static bool action_prompt(const BoardAction *a, int number, char **input) {
    *input = NULL;
    if (a->has_input) return dialog_action_input(app_window(), a, number, input);
    return true;
}
/// The errands this app can start on a row: what the server offers for it, less what the token or server lacks.
static BoardAction *row_actions(const Json *catalog, const PullSummary *pull, int failed_checks, size_t *count) {
    *count = 0;
    if (!store_can_manage()) return NULL;
    size_t n; BoardAction *offered = board_actions_offered(catalog, pull, failed_checks, &n);
    BoardAction *out = xcalloc(n, sizeof *out);
    for (size_t i = 0; i < n; i++) {
        char *op = board_action_operation(&offered[i]);
        if (store_supports(op)) board_action_copy(&out[(*count)++], &offered[i]);
        free(op);
    }
    board_actions_free(offered, n);
    return out;
}

// MARK: - Board

enum { ACT_FILTER = 1000, ACT_TAB, ACT_CLEAR, ACT_OPEN_PULL, ACT_OPEN_ISSUE, ACT_PULL_ACTION, ACT_REFRESH, ACT_FILTER_AUTHOR, ACT_FILTER_REVIEWER, ACT_FILTER_LABEL, ACT_RUNS };
enum { TIMER_POLL = 1 };
enum { ACTION_STRIDE = 64 };   // ACT_PULL_ACTION's argument: row * stride + errand

typedef struct {
    Screen base;
    Project project;
    Json *board;
    PullSummary *pulls; size_t pull_count;
    IssueSummary *issues; size_t issue_count;
    int tab;   // 0 pulls, 1 issues
    Session *runs; size_t run_count; Request *req_runs;   // the project's conversations, for "N runs" on each pull request
    time_t synced_at;
    BoardFilter pull_filter, issue_filter;
    bool loaded, has_opening;
    BoardFilter opening;
    char *error;
    Request *req;
    Poller poller;
    bool fresh_pending;
    Json *catalog;      // the server's `actions`, for the buttons under each pull request
    Request *req_actions, *req_start;
    bool busy, uncertain, dialog_open;
    int starting_number; char *starting_id;
    char *write_error;
} PullsScreen;

static BoardFilter *current_filter(PullsScreen *s) { return s->tab == 0 ? &s->pull_filter : &s->issue_filter; }
static BoardRow *rows_of(PullsScreen *s, size_t *count) {
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
static void filters_save(PullsScreen *s) {
    Json *saved = json_object();
    json_object_set(saved, "pulls", filter_json(&s->pull_filter));
    json_object_set(saved, "issues", filter_json(&s->issue_filter));
    char *key = xstrfmt("pulls-filters:%s", s->project.repo);
    cache_store(g_store.cache, saved, key);
    free(key); json_free(saved);
}
/// A board never filtered opens on the project's author; one with saved picks opens on them.
static bool filters_restore(PullsScreen *s) {
    char *key = xstrfmt("pulls-filters:%s", s->project.repo);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    if (!saved) return false;
    filter_read(&s->pull_filter, json_get(saved, "pulls"));
    filter_read(&s->issue_filter, json_get(saved, "issues"));
    json_free(saved);
    return true;
}

static void pulls_show(PullsScreen *s, const Json *result, bool saved) {
    json_free(s->board); s->board = json_clone(result);
    pull_summaries_free(s->pulls, s->pull_count); s->pulls = pull_summaries_parse(json_get(result, "pulls"), &s->pull_count);
    issue_summaries_free(s->issues, s->issue_count); s->issues = issue_summaries_parse(json_get(result, "issues"), &s->issue_count);
    // A saved board may be out of date about who has something open, so the server's first answer
    // opens the board again, unless the pickers were touched meanwhile.
    if (s->has_opening) {
        size_t n; BoardRow *rows = xcalloc(s->pull_count, sizeof *rows);
        for (n = 0; n < s->pull_count; n++) rows[n] = pull_board_row(&s->pulls[n]);
        BoardFilter filter; board_filter_opening(&filter, json_str(json_get(result, "author")), rows, n);
        free(rows);
        if (board_filter_equal(&s->pull_filter, &s->opening)) { board_filter_free(&s->pull_filter); board_filter_copy(&s->pull_filter, &filter); }
        board_filter_free(&s->opening);
        if (saved) { board_filter_copy(&s->opening, &filter); } else { s->has_opening = false; board_filter_init(&s->opening); }
        board_filter_free(&filter);
    }
    if (!s->issue_count && !json_is_set(json_get(result, "issuesError"))) s->tab = 0;
    s->loaded = true;
}
static void pulls_load(PullsScreen *s, bool fresh);
static void pulls_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (!req->ok) {
        // A server from before `fresh` refuses the argument it does not know.
        if (req->error.kind == API_HTTP && req->error.status == 400 && !json_is_null(json_get(req->args, "fresh"))) { pulls_load(s, false); return; }
        request_error_into(&s->error, req);
        s->loaded = true;
    } else {
        pulls_show(s, req->result, false);
        set_string(&s->error, NULL);
        s->synced_at = time(NULL);
        char *key = xstrfmt("pulls:%s", s->project.repo);
        cache_store(g_store.cache, req->result, key);
        free(key);
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void board_actions_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (!req->ok) return;
    json_free(s->catalog); s->catalog = json_clone(json_get(req->result, "actions"));
    cache_store(g_store.cache, req->result, "actions");
    pane_relayout(s->base.pane);
}
static void runs_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = all; s->run_count = n;
    pane_relayout(s->base.pane);
}
static size_t runs_on(PullsScreen *s, int number) { size_t n = 0; for (size_t i = 0; i < s->run_count; i++) if (session_pull_number(&s->runs[i]) == number) n++; return n; }
static bool run_active_on(PullsScreen *s, int number) { for (size_t i = 0; i < s->run_count; i++) if (session_pull_number(&s->runs[i]) == number && session_is_active(&s->runs[i])) return true; return false; }
static void pulls_load(PullsScreen *s, bool fresh) {
    if (!s->loaded) {
        char *key = xstrfmt("pulls:%s", s->project.repo);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) { pulls_show(s, saved, true); json_free(saved); pane_relayout(s->base.pane); }
        free(key);
        if (!json_count(s->catalog)) { Json *acts = cache_value(g_store.cache, "actions"); if (acts) { json_free(s->catalog); s->catalog = json_clone(json_get(acts, "actions")); json_free(acts); } }
    }
    if (s->req) request_cancel(&s->req);
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo);
    if (fresh) json_set_str(args, "fresh", "1");
    store_call("pulls", args, 0, s, pulls_done, 0, &s->req);
    if (store_can_manage() && store_supports("actions") && !s->req_actions) store_call("actions", json_object(), 0, s, board_actions_done, 0, &s->req_actions);
    if (store_supports("sessions") && !s->req_runs) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, runs_done, 0, &s->req_runs); }
}
static void board_open_served(PullsScreen *s, const Json *result);
static void board_start_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        set_string(&s->write_error, NULL);
        if (str_eq(s->starting_id, "run")) board_open_served(s, req->result);
        // The list stays shown; the new session joins the runs counted on its pull request.
        if (store_supports("sessions")) { request_cancel(&s->req_runs); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, runs_done, 0, &s->req_runs); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (!api_error_is_refusal(&req->error)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}
static void board_start(PullsScreen *s, int number, const char *branch, const BoardAction *action, const char *input) {
    if (s->busy || s->uncertain || str_empty(branch)) return;
    s->busy = true; s->starting_number = number; set_string(&s->starting_id, action->id);
    char *op = board_action_operation(action);
    Json *args = board_action_arguments(action, s->project.repo, number, branch, input);
    store_call(op, args, board_action_timeout_ms(action), s, board_start_done, 0, &s->req_start);
    free(op);
    pane_relayout(s->base.pane);
}

static void pulls_destroy(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_start); poller_stop(&s->poller);
    json_free(s->board); json_free(s->catalog); pull_summaries_free(s->pulls, s->pull_count); issue_summaries_free(s->issues, s->issue_count);
    request_cancel(&s->req_runs); sessions_free(s->runs, s->run_count);
    board_filter_free(&s->pull_filter); board_filter_free(&s->issue_filter); board_filter_free(&s->opening);
    project_free(&s->project); free(s->error); free(s->write_error); free(s->starting_id);
    screen_release(base);
}

/// `#proj-tabs`: 13px, `px-2 py-2`, the open one underlined in the accent.
typedef struct { char *label; bool active; } TabData;
static void tab_free(void *p) { TabData *d = p; free(d->label); free(d); }
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    RECT t = *rc;
    draw_text(cv, d->label, &t, FONT_FOOTNOTE, d->active || hovered ? theme.ink : theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (d->active) { RECT u = { rc->left, rc->bottom - px(2), rc->right, rc->bottom }; fill_rect(cv, &u, theme.accent); }
}
static void doc_tabs(Doc *doc, int w, const char *const *labels, size_t count, int active) {
    int x = 0, h = px(34);
    for (size_t i = 0; i < count; i++) {
        int tw = px(8) * 2 + text_width(doc->cv, labels[i], FONT_FOOTNOTE) + px(2);
        TabData *d = xcalloc(1, sizeof *d); d->label = xstrdup(labels[i]); d->active = (int)i == active;
        RECT rc = { x, doc->y, x + tw, doc->y + h };
        int it = doc_add(doc, &rc, paint_tab);
        doc_item(doc, it)->data = d; doc_item(doc, it)->free_data = tab_free; doc_item(doc, it)->action = ACT_TAB; doc_item(doc, it)->arg = (intptr_t)i; doc_item(doc, it)->hand = true;
        x += tw + px(4);
    }
    doc->y += h;
    doc_rule(doc, -px(18), w + px(36));
}
static void pulls_layout(Screen *base, Doc *doc) {
    PullsScreen *s = (PullsScreen *)base;
    int w = doc->width;
    const char *tabs[2] = { "\xE2\x87\x85 Pull requests", "\xE2\x8A\x99 Issues" };
    doc_tabs(doc, w, tabs, 2, s->tab);
    doc_space(doc, px(14));
    if (s->error) { doc_notice(doc, 0, w, s->error); doc_space(doc, px(10)); }
    if (s->write_error) {
        doc_notice(doc, 0, w, s->write_error);
        if (s->uncertain) { doc_space(doc, px(4)); doc_text(doc, 0, w, "The request may have completed. Refresh (F5) and look for its conversation in the project before starting another agent.", FONT_CAPTION, theme.muted, DT_WORDBREAK); }
        doc_space(doc, px(10));
    }
    BoardFilter *filter = current_filter(s);
    size_t total = s->tab == 0 ? s->pull_count : s->issue_count, shown = 0;
    size_t rn; BoardRow *rows = rows_of(s, &rn);
    for (size_t i = 0; i < rn; i++) if (board_filter_passes(filter, &rows[i], -1)) shown++;
    if (board_filter_is_on(filter)) {
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
            if (!board_filter_passes(filter, &rows[i], -1)) continue;
            const PullSummary *pull = &s->pulls[i];
            StackPosition stack; bool has_stack = stack_position_parse(json_get(pull->raw, "stack"), json_get(s->board, "stacks"), &stack);
            // The dashboard's buttons: the errands its state offers, the suggested one filled. Clicking the row opens the PR.
            size_t an = 0; BoardAction *actions = str_empty(pull->branch) ? NULL : row_actions(s->catalog, pull, 0, &an);
            ButtonSpec *buttons = xcalloc(an + 1, sizeof *buttons); size_t bn = 0;
            char **labels = xcalloc(an + 1, sizeof *labels);
            for (size_t k = 0; k < an && k < ACTION_STRIDE; k++) {
                bool starting = s->busy && s->starting_number == pull->number && str_eq(s->starting_id, actions[k].id);
                bool suggested = str_eq(pull->recommended, actions[k].id);
                labels[k] = starting ? xstrdup("Starting\xE2\x80\xA6") : xstrfmt("%s %s", action_icon(actions[k].id), actions[k].label);
                ButtonSpec b = { 0, labels[k], suggested ? BUTTON_PROMINENT : BUTTON_BORDERED, ACT_PULL_ACTION, (intptr_t)(i * ACTION_STRIDE + k), !s->busy && !s->uncertain };
                buttons[bn++] = b;
            }
            size_t runs = runs_on(s, pull->number);
            char *runs_text = runs ? xstrfmt("%zu run%s \xE2\x80\xBA", runs, runs == 1 ? "" : "s") : NULL;
            if (runs_text) { ButtonSpec b = { 0, runs_text, BUTTON_PLAIN, ACT_RUNS, (intptr_t)i, true }; buttons[bn++] = b; }
            doc_pull_row(doc, 0, w, pull, has_stack ? &stack : NULL, s->project.repo, ACT_OPEN_PULL, (intptr_t)i, buttons, bn, run_active_on(s, pull->number));
            free(runs_text); str_array_free(labels, an); free(buttons); board_actions_free(actions, an);
            if (has_stack) stack_position_free(&stack);
            doc_space(doc, px(8));
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
            for (size_t k = 0; k < nn; k++) {
                int depth = nested[k].depth > 4 ? 4 : nested[k].depth;
                int indent = depth * px(18);
                doc_issue_row(doc, indent, w - indent, &s->issues[map[nested[k].index]], s->project.repo, depth > 0, ACT_OPEN_ISSUE, (intptr_t)map[nested[k].index]);
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
static void pulls_header(Screen *base, HeaderInfo *info) {
    PullsScreen *s = (PullsScreen *)base;
    snprintf(info->title, sizeof info->title, "%s", project_title(&s->project));
    Str sub; str_init(&sub);
    str_appendz(&sub, s->project.repo);
    if (s->loaded) str_appendf(&sub, " \xC2\xB7 %zu open pull request%s", s->pull_count, s->pull_count == 1 ? "" : "s");
    if (s->synced_at) { char *ago = format_relative(s->synced_at); str_appendf(&sub, " \xC2\xB7 synced %s", ago); free(ago); }
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
    str_free(&sub);
    // The pickers, as the dashboard's selects, and ⟳.
    {
        BoardFilter *f = current_filter(s);
        const char *author = board_filter_get(f, FILTER_AUTHOR), *reviewer = board_filter_get(f, FILTER_REVIEWER), *label = board_filter_get(f, FILTER_LABEL);
        HeaderButton *b;
        b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(author) ? "All authors" : author); b->action = ACT_FILTER_AUTHOR; b->enabled = s->loaded;
        if (s->tab == 0) { b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(reviewer) ? "All reviewers" : reviewer); b->action = ACT_FILTER_REVIEWER; b->enabled = s->loaded; }
        b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(label) ? "All labels" : label); b->action = ACT_FILTER_LABEL; b->enabled = s->loaded;
    }
    HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = !s->req; r->tip = "Read the pull requests from GitHub again";
}
/// One picker's menu: `All <kind>s`, then each option with how many rows it would show.
static void filter_pick(PullsScreen *s, FilterKind kind, POINT pt) {
    BoardFilter *filter = current_filter(s);
    size_t rn; BoardRow *rows = rows_of(s, &rn);
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
    if (chosen > 0) { s->has_opening = false; filters_save(s); }
    filter_options_free(options, count);
    free(rows);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void pulls_refresh(Screen *base);
static void pulls_action(Screen *base, int action, intptr_t arg, POINT pt) {
    PullsScreen *s = (PullsScreen *)base;
    switch (action) {
    case ACT_FILTER_AUTHOR: filter_pick(s, FILTER_AUTHOR, pt); break;
    case ACT_FILTER_REVIEWER: filter_pick(s, FILTER_REVIEWER, pt); break;
    case ACT_FILTER_LABEL: filter_pick(s, FILTER_LABEL, pt); break;
    case ACT_REFRESH: pulls_refresh(base); break;
    case ACT_RUNS: if ((size_t)arg < s->pull_count) app_push_detail(pull_detail_screen_new(&s->project, s->pulls[arg].number, NULL, &s->pulls[arg])); break;
    case ACT_TAB: s->tab = arg == 1 ? 1 : 0; pane_relayout(base->pane); pane_header_changed(base->pane); break;
    case ACT_CLEAR: { BoardFilter *f = current_filter(s); board_filter_free(f); board_filter_init(f); s->has_opening = false; filters_save(s); pane_relayout(base->pane); pane_header_changed(base->pane); break; }
    case ACT_OPEN_PULL: {
        if ((size_t)arg >= s->pull_count) break;
        StackPosition stack; bool has_stack = stack_position_parse(json_get(s->pulls[arg].raw, "stack"), json_get(s->board, "stacks"), &stack);
        app_push_detail(pull_detail_screen_new(&s->project, s->pulls[arg].number, has_stack ? &stack : NULL, &s->pulls[arg]));
        if (has_stack) stack_position_free(&stack);
        break;
    }
    case ACT_OPEN_ISSUE: if ((size_t)arg < s->issue_count) app_push_detail(issue_detail_screen_new(&s->project, &s->issues[arg])); break;
    case ACT_PULL_ACTION: {
        size_t index = (size_t)arg / ACTION_STRIDE, k = (size_t)arg % ACTION_STRIDE;
        if (index >= s->pull_count || s->busy || s->uncertain) break;
        // The rows may be replaced while the prompt is up, so what the start needs is copied first.
        int number = s->pulls[index].number; char *branch = xstrdup(s->pulls[index].branch ? s->pulls[index].branch : "");
        size_t an; BoardAction *actions = row_actions(s->catalog, &s->pulls[index], 0, &an);
        if (k < an) {
            char *input = NULL;
            s->dialog_open = true;
            bool ok = action_prompt(&actions[k], number, &input);
            s->dialog_open = false;
            if (ok) board_start(s, number, branch, &actions[k], input);
            free(input);
        }
        board_actions_free(actions, an); free(branch);
        break;
    }
    }
}
static void pulls_timer(Screen *base, UINT id) {
    PullsScreen *s = (PullsScreen *)base;
    if (poller_fired(&s->poller, id)) { if (s->dialog_open) poller_finished(&s->poller, false, -1); else pulls_load(s, false); }
}
static void pulls_visible(Screen *base, bool shown) {
    PullsScreen *s = (PullsScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 45000);
    else { poller_stop(&s->poller); request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_runs); }
}
static void pulls_refresh(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    // Refreshing is how an uncertain start is checked: its conversation is listed in the project if it began.
    s->uncertain = false; set_string(&s->write_error, NULL);
    pulls_load(s, true);
}
static void pulls_activated(Screen *base, bool active) { if (active) pulls_visible(base, true); }
static const ScreenVTable pulls_vt = {
    .destroy = pulls_destroy, .layout = pulls_layout, .header = pulls_header, .action = pulls_action, .timer = pulls_timer,
    .visible = pulls_visible, .refresh = pulls_refresh, .activated = pulls_activated,
};
Screen *pulls_screen_new(const Project *project) {
    PullsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pulls_vt; s->base.id = xstrfmt("pulls:%s", project->repo);
    project_copy(&s->project, project);
    s->board = json_null(); s->catalog = json_array();
    board_filter_init(&s->pull_filter); board_filter_init(&s->issue_filter); board_filter_init(&s->opening);
    s->has_opening = !filters_restore(s);
    return &s->base;
}

// MARK: - Pull request

enum {
    ACT_OPEN_URL = 1100, ACT_STACK_ITEM, ACT_STACK_TOGGLE, ACT_PR_TAB, ACT_FINDING_TOGGLE, ACT_FINDING_DECISION, ACT_MERGE,
    ACT_START_ACTION, ACT_OPEN_RUN, ACT_CHECK_URL, ACT_COMMIT_URL, ACT_ISSUE_URL, ACT_FINDING_URL, ACT_RELOAD, ACT_SOLVE_FINDINGS, ACT_CONV_URL,
    ACT_DELETE_RUN, ACT_WEB_RELOAD, ACT_WEB_BROWSER,
    ACT_FILES_BASE = 1200,   // the Files changed tab's own actions, PULL_FILES_ACTIONS of them
};
enum { TIMER_FILES_PAGE = 2 };
enum { TAG_PULL = 1, TAG_FINDINGS, TAG_ROWS, TAG_ACTIONS, TAG_SESSIONS, TAG_START, TAG_MERGE, TAG_DECIDE, TAG_BODY, TAG_CONV, TAG_DELETE_RUN };
enum { PR_TAB_BODY, PR_TAB_CONVERSATION, PR_TAB_SESSIONS, PR_TAB_FILES, PR_TAB_COMMITS, PR_TAB_CHECKS, PR_TAB_FINDINGS, PR_TAB_RUN };

/// One of the Conversation tab's lists, read page by page: `incoming` fills up and replaces `items` once the last page is in.
enum { CONV_COMMENTS, CONV_REVIEWS, CONV_REVIEW_COMMENTS, CONV_FEEDS };
static const struct { const char *op, *field; } conv_feeds[CONV_FEEDS] = {
    { "pull_comments", "comments" }, { "pull_reviews", "reviews" }, { "pull_review_comments", "reviewComments" },
};
typedef struct { Json *items, *incoming; bool read; char *error; Request *req; } ConvFeed;

typedef struct {
    Screen base;
    Project project; int number;
    bool has_stack; StackPosition stack; bool stack_open;   // the overview under the title, as GitHub's stack popover
    bool has_summary; PullSummary summary;
    Json *pr;
    bool has_row; PullSummary row; bool row_read;
    Json *catalog;      // the server's `actions`
    Session *runs; size_t run_count;
    char *deleting_run, *run_error;   // the conversation being deleted from the Sessions tab, and why the last one failed
    Json *findings;
    PullFiles *files;   // the Files changed tab
    ConvFeed conv[CONV_FEEDS];   // the Conversation tab
    char *error, *findings_error, *write_error, *merge_error;
    bool busy, uncertain, merging, serving;   // serving: the errand under way is ▶ Run, whose page opens when it answers
    // The Run tab: where ▶ Run serves the pull request, in an embedded browser laid over the tab's area (`web_rc`, in
    // document coordinates) while the tab is open.
    char *run_url; WebView *web; RECT web_rc; bool shown;
    int tab;
    char *body, *body_author;   // the description, from `pull_files` when `pull` leaves it out
    bool body_read;
    char *deciding;
    int *open_findings; size_t open_finding_count;
    BoardAction *actions; size_t action_count;
    Request *req_pull, *req_findings, *req_rows, *req_actions, *req_sessions, *req_start, *req_merge, *req_decide, *req_body, *req_delete_run;
    Poller poller;
    bool dialog_open;
} PullScreen;

static const PullSummary *board_row(PullScreen *s) { return s->row_read ? (s->has_row ? &s->row : NULL) : (s->has_row ? &s->row : (s->has_summary ? &s->summary : NULL)); }
static bool is_open(PullScreen *s) { return json_is_null(s->pr) ? board_row(s) != NULL : str_eq(json_str(json_get(s->pr, "state")), "open"); }
static void pull_load(PullScreen *s);

static void save_pull(PullScreen *s) {
    if (json_is_null(s->pr)) return;
    Json *saved = json_object();
    json_object_set(saved, "pr", json_clone(s->pr));
    json_object_set(saved, "findings", json_clone(s->findings));
    const PullSummary *row = board_row(s);
    json_object_set(saved, "row", row ? json_clone(row->raw) : json_null());
    json_object_set(saved, "stack", s->has_stack ? stack_position_json(&s->stack) : json_null());
    if (s->body) json_set_str(saved, "body", s->body);
    if (s->body_author) json_set_str(saved, "bodyAuthor", s->body_author);
    char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
    cache_store(g_store.cache, saved, key);
    json_free(saved); free(key);
}
static void rebuild_actions(PullScreen *s) {
    board_actions_free(s->actions, s->action_count); s->actions = NULL; s->action_count = 0;
    if (!is_open(s) || !json_str(json_get(s->pr, "headRef"))) return;
    s->actions = row_actions(s->catalog, board_row(s), json_int_or(json_get(json_get(s->pr, "checks"), "failed"), 0), &s->action_count);
}
static void pull_finish_poll(PullScreen *s) {
    if (s->req_pull || s->req_findings) return;
    rebuild_actions(s);
    save_pull(s);
    poller_finished(&s->poller, s->error != NULL, -1);
    pane_relayout(s->base.pane);
}
static void findings_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); set_string(&s->findings_error, NULL); }
    else { request_error_into(&s->findings_error, req); }
    pull_finish_poll(s);
}
static void body_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->body_read = true;
    if (req->ok) {
        const Json *pr = json_get(req->result, "pr");
        const char *body = json_str(json_get(pr, "body"));
        set_string(&s->body, body ? body : "");
        if (json_str(json_get(pr, "author"))) set_string(&s->body_author, json_str(json_get(pr, "author")));
        save_pull(s);
    }
    pane_relayout(s->base.pane);
}
static void conv_page(PullScreen *s, int k, int page);
static void conv_done(void *owner, Request *req) {
    PullScreen *s = owner;
    int k = 0;
    while (k < CONV_FEEDS && !str_eq(req->operation, conv_feeds[k].op)) k++;
    if (k == CONV_FEEDS) return;
    ConvFeed *f = &s->conv[k];
    if (!req->ok) {
        request_error_into(&f->error, req);
        json_free(f->incoming); f->incoming = NULL; f->read = true;
        pane_relayout(s->base.pane);
        return;
    }
    const Json *rows = json_get(req->result, conv_feeds[k].field);
    for (size_t i = 0; i < json_count(rows); i++) json_array_push(f->incoming, json_clone(json_at(rows, i)));
    double next;
    if (json_num(json_get(req->result, "nextPage"), &next) && next > json_int_or(json_get(req->args, "page"), 1)) { conv_page(s, k, (int)next); return; }
    json_free(f->items); f->items = f->incoming; f->incoming = NULL;
    f->read = true; set_string(&f->error, NULL);
    pane_relayout(s->base.pane);
}
static void conv_page(PullScreen *s, int k, int page) {
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_num(args, "page", page);
    store_call(conv_feeds[k].op, args, 0, s, conv_done, TAG_CONV, &s->conv[k].req);
}
/// Reads the comments, reviews and line comments afresh; what was read stays shown until the new lists are in.
static void conv_load(PullScreen *s) {
    for (int k = 0; k < CONV_FEEDS; k++) {
        ConvFeed *f = &s->conv[k];
        if (f->req || !store_supports(conv_feeds[k].op)) continue;
        json_free(f->incoming); f->incoming = json_array();
        conv_page(s, k, 1);
    }
}
static void conv_cancel(PullScreen *s) { for (int k = 0; k < CONV_FEEDS; k++) request_cancel(&s->conv[k].req); }

static void pull_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); pull_finish_poll(s); return; }
    json_free(s->pr); s->pr = json_clone(json_get(req->result, "pr"));
    set_string(&s->error, NULL);
    // The description is its own read, once per visit and on refresh.
    if (!json_str(json_get(s->pr, "body")) && !s->body_read && !s->req_body && store_supports("pull_description")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("pull_description", args, 0, s, body_done, TAG_BODY, &s->req_body);
    }
    if (store_supports("findings")) {
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
        store_call("findings", args, 0, s, findings_done, TAG_FINDINGS, &s->req_findings);
    }
    pull_finish_poll(s);
}
static void rows_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    // A pull request the board no longer lists has been merged or closed, and its row went with it.
    if (s->has_row) pull_summary_free(&s->row);
    s->has_row = false;
    size_t count;
    PullSummary *rows = pull_summaries_parse(json_get(req->result, "pulls"), &count);
    for (size_t i = 0; i < count; i++) {
        if (rows[i].number != s->number) continue;
        pull_summary_copy(&s->row, &rows[i]); s->has_row = true;
        // Its place in a stack, with the branches of the rows it is built on; a row no longer stacked drops the overview.
        StackPosition stack;
        bool stacked = stack_position_parse(json_get(rows[i].raw, "stack"), json_get(req->result, "stacks"), &stack);
        if (stacked) stack_position_branches(&stack, rows, count);
        if (s->has_stack) stack_position_free(&s->stack);
        s->has_stack = stacked;
        if (stacked) s->stack = stack;
        break;
    }
    pull_summaries_free(rows, count);
    s->row_read = true;
    rebuild_actions(s);
    pane_relayout(s->base.pane);
}
static void actions_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    json_free(s->catalog); s->catalog = json_clone(json_get(req->result, "actions"));
    cache_store(g_store.cache, req->result, "actions");
    rebuild_actions(s);
    pane_relayout(s->base.pane);
}
static void sessions_done_pull(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = xcalloc(n, sizeof *s->runs); s->run_count = 0;
    for (size_t i = 0; i < n; i++) if (session_pull_number(&all[i]) == s->number) session_copy(&s->runs[s->run_count++], &all[i]);
    sessions_free(all, n);
    pane_relayout(s->base.pane);
}
static void pull_load(PullScreen *s) {
    if (json_is_null(s->pr)) {
        char *key = xstrfmt("pull:%s#%d", s->project.repo, s->number);
        Json *saved = cache_value(g_store.cache, key);
        if (saved) {
            json_free(s->pr); s->pr = json_clone(json_get(saved, "pr"));
            json_free(s->findings); s->findings = json_clone(json_get(saved, "findings"));
            if (!s->body && json_str(json_get(saved, "body"))) set_string(&s->body, json_str(json_get(saved, "body")));
            if (!s->body_author && json_str(json_get(saved, "bodyAuthor"))) set_string(&s->body_author, json_str(json_get(saved, "bodyAuthor")));
            if (!s->has_row) { PullSummary row; if (pull_summary_parse(json_get(saved, "row"), &row)) { s->row = row; s->has_row = true; } }
            if (!s->has_stack) s->has_stack = stack_position_restore(json_get(saved, "stack"), &s->stack);
            json_free(saved);
        }
        free(key);
        if (!json_count(s->catalog)) { Json *acts = cache_value(g_store.cache, "actions"); if (acts) { json_free(s->catalog); s->catalog = json_clone(json_get(acts, "actions")); json_free(acts); } }
        rebuild_actions(s);
        pane_relayout(s->base.pane);
    }
    if (s->req_pull) return;
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    store_call("pull", args, 0, s, pull_done, TAG_PULL, &s->req_pull);
    // What the board knows about this pull request beyond its own details.
    if (store_supports("pulls")) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("pulls", a, 0, s, rows_done, TAG_ROWS, &s->req_rows); }
    if (store_can_manage() && store_supports("actions")) store_call("actions", json_object(), 0, s, actions_done, TAG_ACTIONS, &s->req_actions);
    if (store_supports("sessions")) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    if (s->tab == PR_TAB_CONVERSATION) conv_load(s);
}

static void pull_destroy(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_rows); request_cancel(&s->req_actions);
    request_cancel(&s->req_sessions); request_cancel(&s->req_start); request_cancel(&s->req_merge); request_cancel(&s->req_decide); request_cancel(&s->req_body);
    request_cancel(&s->req_delete_run); free(s->deleting_run); free(s->run_error);
    poller_stop(&s->poller);
    project_free(&s->project); if (s->has_stack) stack_position_free(&s->stack); if (s->has_summary) pull_summary_free(&s->summary);
    json_free(s->pr); if (s->has_row) pull_summary_free(&s->row); json_free(s->catalog); sessions_free(s->runs, s->run_count); json_free(s->findings);
    free(s->error); free(s->findings_error); free(s->write_error); free(s->merge_error); free(s->deciding); free(s->open_findings); free(s->body); free(s->body_author);
    board_actions_free(s->actions, s->action_count);
    pull_files_free(s->files);
    webview_free(s->web); free(s->run_url);
    for (int k = 0; k < CONV_FEEDS; k++) { request_cancel(&s->conv[k].req); json_free(s->conv[k].items); json_free(s->conv[k].incoming); free(s->conv[k].error); }
    screen_release(base);
}

static bool finding_open(PullScreen *s, int i) { for (size_t k = 0; k < s->open_finding_count; k++) if (s->open_findings[k] == i) return true; return false; }
static const char *const decision_ids[] = { "", "fix", "optional", "dismissed" };
static const char *const decision_titles[] = { "Undecided", "Fix", "Optional", "Dismiss" };

typedef struct { wchar_t glyph; COLORREF color; char *name, *result; } CheckData;
static void check_free(void *p) { CheckData *d = p; free(d->name); free(d->result); free(d); }
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CheckData *d = it->data;
    RECT g = { rc->left, rc->top, rc->left + px(18), rc->bottom }; draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->color);
    int rw = text_width(cv, d->result, FONT_CALLOUT);
    RECT n = { rc->left + px(22), rc->top, rc->right - rw - px(8), rc->bottom }; draw_text(cv, d->name, &n, FONT_CALLOUT, it->action ? theme.accent : theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT r = { rc->right - rw, rc->top, rc->right, rc->bottom }; draw_text(cv, d->result, &r, FONT_CALLOUT, theme.secondary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
typedef struct { int passed, failed, pending; } CountsData;
static void paint_counts(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CountsData *d = it->data;
    int x = rc->left;
    struct { wchar_t g; int n; COLORREF c; } parts[3] = { { 0xE930, d->passed, theme.success }, { 0xE823, d->pending, theme.warning }, { 0xEA39, d->failed, theme.danger } };
    for (int i = 0; i < 3; i++) {
        RECT g = { x, rc->top, x + px(18), rc->bottom }; draw_glyph(cv, parts[i].g, &g, FONT_ICON_SMALL, parts[i].c);
        char n[16]; snprintf(n, sizeof n, "%d", parts[i].n);
        int nw = text_width(cv, n, FONT_SUBHEADLINE_SEMIBOLD);
        RECT t = { x + px(20), rc->top, x + px(20) + nw, rc->bottom }; draw_text(cv, n, &t, FONT_SUBHEADLINE_SEMIBOLD, parts[i].c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += px(20) + nw + px(14);
    }
}

static int section_box(Doc *doc, int w) { int b = doc_box_begin(doc, 0, w, px(12), theme.raise, theme.line, px(8)); doc_item(doc, b)->hover_fill = false; return b; }
static void row_gap(Doc *doc, int w) { doc_space(doc, px(6)); doc_rule(doc, px(12), w - px(24)); doc_space(doc, px(6)); }

// A column of the pull request screen: the whole width, or one of two side by side when the pane is wide.
typedef struct { int x, w, ix, iw; } Col;
static Col col_make(int x, int w) { Col c = { x, w, x + px(12), w - px(24) }; return c; }
static int col_box(Doc *doc, Col c) { int b = doc_box_begin(doc, c.x, c.w, px(12), theme.raise, theme.line, px(8)); doc_item(doc, b)->hover_fill = false; return b; }
static void col_gap(Doc *doc, Col c) { doc_space(doc, px(6)); doc_rule(doc, c.ix, c.iw); doc_space(doc, px(6)); }

/// `.prv-state`: a filled pill, capitalised, green for open, the accent for merged, red for closed, grey for a draft.
typedef struct { char *text; COLORREF color; } PillData;
static void pill_free(void *p) { PillData *d = p; free(d->text); free(d); }
static void paint_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    PillData *d = it->data;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, d->color, d->color);
    RECT t = *rc;
    draw_text(cv, d->text, &t, FONT_FOOTNOTE_SEMIBOLD, theme.canvas, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
static int doc_pill(Doc *doc, int x, int y, const char *text, COLORREF color) {
    PillData *d = xcalloc(1, sizeof *d); d->text = str_capitalized(text); d->color = color;
    int w = px(12) * 2 + text_width(doc->cv, d->text, FONT_FOOTNOTE_SEMIBOLD) + 2, h = px(26);
    RECT rc = { x, y, x + w, y + h };
    int i = doc_add(doc, &rc, paint_pill);
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = pill_free;
    return w;
}
/// GitHub's stack button beside the state: the stack glyph and `2/3` in a bordered pill, pressed while the overview is open.
static void paint_stack_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    PillData *d = it->data;
    bool open = it->arg != 0, hovered = doc_item_hovered(doc, it);
    COLORREF fill = open ? blend(theme.accent, theme.canvas, 0.18) : hovered ? theme.raise : theme.canvas;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, fill, open ? theme.accent : theme.line);
    RECT g = { rc->left + px(10), rc->top, rc->left + px(10) + px(16), rc->bottom }; draw_glyph(cv, 0xE81E, &g, FONT_ICON_SMALL, theme.accent);
    RECT t = { g.right + px(4), rc->top, rc->right - px(10), rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
static int doc_stack_pill(Doc *doc, int x, int y, const char *text, bool open) {
    PillData *d = xcalloc(1, sizeof *d); d->text = xstrdup(text); d->color = theme.accent;
    int w = px(10) * 2 + px(16) + px(4) + text_width(doc->cv, text, FONT_FOOTNOTE_SEMIBOLD) + 2, h = px(26);
    RECT rc = { x, y, x + w, y + h };
    int i = doc_add(doc, &rc, paint_stack_pill);
    Item *it = doc_item(doc, i);
    it->data = d; it->free_data = pill_free; it->action = ACT_STACK_TOGGLE; it->arg = open; it->hand = true;
    return w;
}
/// One pull request of the overview: the pull request glyph, its title, and `#number · branch` under it, as GitHub's popover rows.
typedef struct { char *title, *sub; bool current; } StackRowData;
static void stack_row_free(void *p) { StackRowData *d = p; free(d->title); free(d->sub); free(d); }
static void paint_stack_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    StackRowData *d = it->data;
    bool hovered = it->action && doc_item_hovered(doc, it);
    if (hovered || d->current) { RECT h = { rc->left, rc->top, rc->right, rc->bottom }; fill_round_rect(cv, &h, px(6), d->current ? blend(theme.accent, theme.raise, 0.10) : theme.canvas, d->current ? blend(theme.accent, theme.raise, 0.10) : theme.canvas); }
    if (d->current) { RECT bar = { rc->left, rc->top + px(6), rc->left + px(3), rc->bottom - px(6) }; fill_round_rect(cv, &bar, px(1), theme.accent, theme.accent); }
    int x = rc->left + px(10);
    RECT g = { x, rc->top, x + px(18), rc->top + px(22) }; draw_glyph(cv, 0xE81E, &g, FONT_ICON_SMALL, d->current ? theme.ink : theme.accent); x += px(18) + px(8);
    RECT t = { x, rc->top + px(2), rc->right - px(8), rc->top + px(22) };
    draw_text(cv, d->title, &t, FONT_FOOTNOTE_SEMIBOLD, d->current ? theme.ink : hovered ? theme.accent : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // The rail of the stack: a thin line under the glyph joining the rows.
    draw_line(cv, rc->left + px(10) + px(9), rc->top + px(22), rc->left + px(10) + px(9), rc->bottom, theme.line);
    RECT u = { x, rc->top + px(22), rc->right - px(8), rc->bottom - px(2) };
    draw_text(cv, d->sub, &u, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// The base the bottom merges into, as GitHub closes its popover: a hollow dot and the branch in a chip.
typedef struct { char *base; } StackBaseData;
static void stack_base_free(void *p) { StackBaseData *d = p; free(d->base); free(d); }
static void paint_stack_base(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    StackBaseData *d = it->data;
    int cx = rc->left + px(10) + px(9), cy = rc->top + (rc->bottom - rc->top) / 2;
    draw_line(cv, cx, rc->top, cx, cy - px(5), theme.line);
    stroke_circle(cv, cx, cy, px(4), theme.muted, 1);
    int x = rc->left + px(10) + px(18) + px(8);
    if (d->base) { int h; draw_chip(cv, x, cy - px(10), d->base, theme.accent, theme.raise, &h); }
    else { RECT t = { x, rc->top, rc->right - px(8), rc->bottom }; draw_text(cv, "base branch not on the board", &t, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); }
}
/// GitHub's stack popover, laid out under the title: the stack top first, this pull request marked, and the base branch at the bottom.
static void layout_stack_overview(PullScreen *s, Doc *doc, Col c) {
    int w = c.iw < px(520) ? c.iw : px(520);
    int box = doc_box_begin(doc, c.ix, w, px(10), theme.raise, theme.line, px(10));
    int ix = c.ix + px(12), iw = w - px(24);
    char *label = stack_position_label(&s->stack, s->number);
    char *title = xstrfmt("Stack \xC2\xB7 %s", label);
    doc_text(doc, ix + px(4), iw - px(4), title, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title); free(label);
    doc_space(doc, px(6)); doc_rule(doc, ix, iw); doc_space(doc, px(6));
    size_t *order = stack_position_top_first(&s->stack);
    for (size_t k = 0; k < s->stack.chain_count; k++) {
        const StackItem *item = &s->stack.chain[order[k]];
        StackRowData *d = xcalloc(1, sizeof *d);
        d->current = item->number == s->number;
        d->title = xstrdup(item->title);
        d->sub = xstrfmt("#%d%s%s%s", item->number, item->branch ? " \xC2\xB7 " : "", item->branch ? item->branch : "", item->draft ? " \xC2\xB7 draft" : "");
        doc_custom(doc, ix, iw, px(44), paint_stack_row, d, stack_row_free, d->current ? 0 : ACT_STACK_ITEM, item->number);
    }
    free(order);
    if (!s->stack.chain_count) doc_text(doc, ix + px(4), iw - px(4), "The board did not list the stack's pull requests.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    StackBaseData *b = xcalloc(1, sizeof *b);
    b->base = xstrdup(s->stack.base);
    doc_custom(doc, ix, iw, px(30), paint_stack_base, b, stack_base_free, 0, 0);
    doc_space(doc, px(4));
    doc_text(doc, ix + px(4), iw - px(4), s->stack.partial ? "Only part of this stack is on the board; it may be longer. Merge from the bottom up." : "Merge from the bottom up.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    doc_box_end(doc, box, px(10));
}
/// Text that wraps a word at a time and mixes fonts, colours and branch chips: the title with its muted number, and the line under it.
typedef struct { char *text; FontId font; COLORREF color; bool chip; int x, y, w; } FlowRun;
typedef struct { FlowRun *runs; size_t count; int line_h; } FlowData;
static void flow_free(void *p) { FlowData *d = p; for (size_t i = 0; i < d->count; i++) free(d->runs[i].text); free(d->runs); free(d); }
static FlowData *flow_new(int line_h) { FlowData *d = xcalloc(1, sizeof *d); d->line_h = line_h; return d; }
static void flow_push(FlowData *d, const char *text, size_t n, FontId font, COLORREF color, bool chip) {
    d->runs = xrealloc(d->runs, (d->count + 1) * sizeof *d->runs);
    FlowRun r = { xstrndup(text, n), font, color, chip, 0, 0, 0 };
    d->runs[d->count++] = r;
}
/// Plain text goes in word by word, each word keeping its trailing space; a chip goes in whole.
static void flow_add(FlowData *d, const char *text, FontId font, COLORREF color, bool chip) {
    if (chip) { flow_push(d, text, strlen(text), font, color, true); return; }
    for (const char *p = text; *p;) {
        const char *e = p;
        while (*e && *e != ' ') e++;
        while (*e == ' ') e++;
        flow_push(d, p, (size_t)(e - p), font, color, false);
        p = e;
    }
}
static void paint_flow(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    FlowData *d = it->data;
    for (size_t i = 0; i < d->count; i++) {
        const FlowRun *r = &d->runs[i];
        RECT t = { rc->left + r->x, rc->top + r->y, rc->left + r->x + r->w, rc->top + r->y + d->line_h };
        if (r->chip) {
            // `.commit-ref`: the branch in small mono type on a tint of the accent.
            int ch = font_height(cv, r->font) + px(6);
            RECT c = { t.left, t.top + (d->line_h - ch) / 2, t.right, t.top + (d->line_h - ch) / 2 + ch };
            COLORREF tint = blend(theme.accent, theme.canvas, 0.16);
            fill_round_rect(cv, &c, px(6), tint, tint);
            draw_text(cv, r->text, &c, r->font, theme.accent, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else draw_text(cv, r->text, &t, r->font, r->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}
/// Lays the runs out in `w`, the first line starting `indent` in, as one item.
static int doc_flow(Doc *doc, int x, int w, int indent, FlowData *d) {
    int cx = indent, cy = 0;
    for (size_t i = 0; i < d->count; i++) {
        FlowRun *r = &d->runs[i];
        r->w = text_width(doc->cv, r->text, r->font) + (r->chip ? px(12) : 0);
        if (cx > 0 && cx + r->w > w) { cx = 0; cy += d->line_h; }
        if (r->w > w) r->w = w;
        r->x = cx; r->y = cy;
        cx += r->w + (r->chip ? px(5) : 0);
    }
    RECT rc = { x, doc->y, x + w, doc->y + cy + d->line_h };
    int i = doc_add(doc, &rc, paint_flow);
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = flow_free;
    doc->y = rc.bottom;
    return i;
}

static int toolbar_width(Canvas *cv, const ButtonSpec *buttons, size_t count) {
    int w = 0;
    for (size_t i = 0; i < count; i++) w += px(10) * 2 + text_width(cv, buttons[i].text, FONT_FOOTNOTE) + 2 + (i ? px(6) : 0);
    return w;
}
static const char *pull_author(PullScreen *s) {
    const PullSummary *row = board_row(s);
    const char *author = json_str(json_get(s->pr, "author"));
    if (!author && row) author = row->author;
    return author ? author : s->body_author;
}
static int pull_commit_count(PullScreen *s) { return json_int_or(json_get(s->pr, "commits"), (int)json_count(json_get(s->pr, "commitList"))); }

/// `gh-header`: the title with its muted number and the buttons beside it, then the state and `author wants to merge N commits into base from head`.
static void layout_header(PullScreen *s, Doc *doc, Col c) {
    const PullSummary *row = board_row(s);
    bool can_merge = store_supports("merge_pull") && str_eq(json_str(json_get(s->pr, "state")), "open") && json_bool_tristate(json_get(s->pr, "draft")) != 1
        && json_str(json_get(s->pr, "headSha")) && json_str(json_get(s->pr, "baseRef"));
    ButtonSpec buttons[3]; size_t bn = 0;
    { ButtonSpec b = { 0, "\xE2\x9F\xB3 Refresh", BUTTON_BORDERED, ACT_RELOAD, 0, !s->req_pull }; buttons[bn++] = b; }
    if (can_merge) { ButtonSpec b = { 0, s->merging ? "Merging\xE2\x80\xA6" : "Merge", BUTTON_PROMINENT, ACT_MERGE, 0, !s->merging && !s->busy }; buttons[bn++] = b; }
    if (safe_web_url(json_str(json_get(s->pr, "url")))) { ButtonSpec b = { 0, "Open in GitHub \xE2\x86\x97", BUTTON_BORDERED, ACT_OPEN_URL, 0, true }; buttons[bn++] = b; }
    int tw = toolbar_width(doc->cv, buttons, bn) + px(2);
    int title_line = font_height(doc->cv, FONT_TITLE) + px(6);
    // Beside the title when there is room, above it when there is not.
    bool beside = c.iw - tw - px(16) >= px(320);
    int top = doc->y, buttons_bottom = top;
    if (beside) { doc->y = top + px(3); doc_button_row(doc, c.ix + c.iw - tw, tw, buttons, bn); buttons_bottom = doc->y; doc->y = top; }
    else { doc_button_row(doc, c.ix, c.iw, buttons, bn); doc_space(doc, px(12)); }
    const char *title = json_str(json_get(s->pr, "title"));
    if (!title && row) title = row->title;
    FlowData *t = flow_new(title_line);
    char *spaced = xstrfmt("%s ", title ? title : "Pull request"); flow_add(t, spaced, FONT_TITLE, theme.ink, false); free(spaced);
    char *number = xstrfmt("#%d", s->number); flow_add(t, number, FONT_TITLE, theme.muted, false); free(number);
    doc_flow(doc, c.ix, beside ? c.iw - tw - px(16) : c.iw, 0, t);
    if (doc->y < buttons_bottom) doc->y = buttons_bottom;
    doc_space(doc, px(10));
    // The state pill, then the sentence GitHub writes under the title.
    bool draft = json_bool_tristate(json_get(s->pr, "draft")) == 1 || (json_is_null(s->pr) && row && row->draft);
    const char *state = json_str(json_get(s->pr, "state"));
    const char *state_text = draft && is_open(s) ? "draft" : state ? state : row ? "open" : "loading";
    COLORREF state_color = str_eq(state, "merged") ? theme.accent : str_eq(state, "closed") ? theme.danger : draft ? theme.muted : theme.ok;
    int y = doc->y;
    int pw = doc_pill(doc, c.ix, y, state_text, state_color);
    if (s->has_stack) { char *label = stack_position_label(&s->stack, s->number); pw += px(8) + doc_stack_pill(doc, c.ix + pw + px(8), y, label, s->stack_open); free(label); }
    const char *head = json_str(json_get(s->pr, "headRef")), *base_ref = json_str(json_get(s->pr, "baseRef"));
    if (!head && row && *row->branch) head = row->branch;
    if (!base_ref && row && *row->base_branch) base_ref = row->base_branch;
    const char *author = pull_author(s);
    int commits = pull_commit_count(s);
    FlowData *f = flow_new(px(26));
    if (author) { char *a = xstrfmt("%s ", author); flow_add(f, a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(a); }
    if (head || base_ref || commits > 0) {
        bool done = str_eq(state, "merged");
        Str verb; str_init(&verb);
        str_appendz(&verb, done ? "merged " : author ? "wants to merge " : "Wants to merge ");
        if (done && !author) verb.data[0] = 'M';
        if (commits > 0) str_appendf(&verb, "%d commit%s ", commits, commits == 1 ? "" : "s");
        if (base_ref || head) str_appendz(&verb, "into ");
        flow_add(f, verb.data, FONT_FOOTNOTE, theme.muted, false);
        str_free(&verb);
        if (base_ref || head) flow_add(f, base_ref ? base_ref : "main", FONT_MONO_CAPTION2, theme.accent, true);
        if (head) { flow_add(f, "from ", FONT_FOOTNOTE, theme.muted, false); flow_add(f, head, FONT_MONO_CAPTION2, theme.accent, true); }
    }
    if (row && str_eq(row->mergeable, "conflicting")) { char *w = xstrfmt("\xC2\xB7 \xE2\x9A\xA0 conflicts with %s ", base_ref ? base_ref : "its base"); flow_add(f, w, FONT_FOOTNOTE, theme.warning, false); free(w); }
    if (row && row->has_updated) { char *rel = format_relative(row->updated_at); char *u = xstrfmt("\xC2\xB7 updated %s", rel); flow_add(f, u, FONT_FOOTNOTE, theme.muted, false); free(u); free(rel); }
    if (f->count) doc_flow(doc, c.ix + pw + px(10), c.iw - pw - px(10), 0, f); else flow_free(f);
    if (doc->y < y + px(26)) doc->y = y + px(26);
    if (s->has_stack && s->stack_open) { doc_space(doc, px(10)); layout_stack_overview(s, doc, c); }
    if (s->merge_error) { doc_space(doc, px(8)); doc_notice(doc, c.ix, c.iw, s->merge_error); }
}

/// Whether every list the server offers has been read once.
static bool conv_read(PullScreen *s) {
    for (int k = 0; k < CONV_FEEDS; k++) if (store_supports(conv_feeds[k].op) && !s->conv[k].read) return false;
    return true;
}
static size_t conv_count(PullScreen *s);

/// `tabnav`: Sessions, PR Body, Conversation, Files changed, Commits, Checks and Findings with their counts (doc_tab),
/// and the diffstat at the right.
typedef struct { int additions, deletions; } DiffStatData;
static int diffstat_width(Canvas *cv, const DiffStatData *d) {
    char add[16], del[24]; snprintf(add, sizeof add, "+%d", d->additions); snprintf(del, sizeof del, "\xE2\x88\x92%d", d->deletions);
    return text_width(cv, add, FONT_CAPTION_SEMIBOLD) + px(4) + text_width(cv, del, FONT_CAPTION_SEMIBOLD) + px(6) + 5 * px(10);
}
static void paint_diffstat(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    DiffStatData *d = it->data;
    char add[16], del[24]; snprintf(add, sizeof add, "+%d", d->additions); snprintf(del, sizeof del, "\xE2\x88\x92%d", d->deletions);
    int x = rc->left, mid = rc->bottom - px(2);
    int aw = text_width(cv, add, FONT_CAPTION_SEMIBOLD), dw = text_width(cv, del, FONT_CAPTION_SEMIBOLD);
    RECT a = { x, rc->top, x + aw, mid }; draw_text(cv, add, &a, FONT_CAPTION_SEMIBOLD, theme.success, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += aw + px(4);
    RECT r = { x, rc->top, x + dw, mid }; draw_text(cv, del, &r, FONT_CAPTION_SEMIBOLD, theme.danger, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += dw + px(6);
    // `.diffstat-block-*`: five squares split by the share of lines added and deleted.
    long total = (long)d->additions + d->deletions;
    int green = total ? (int)(5L * d->additions / total) : 0, red = total ? (int)(5L * d->deletions / total) : 0;
    int sq = px(8), top = rc->top + (mid - rc->top - sq) / 2;
    for (int i = 0; i < 5; i++) {
        RECT b = { x, top, x + sq, top + sq };
        COLORREF color = i < green ? theme.success : i < green + red ? theme.danger : theme.line;
        fill_round_rect(cv, &b, px(2), color, color);
        x += sq + px(2);
    }
}
static void layout_tabs(PullScreen *s, Doc *doc, Col c) {
    int h = px(42), x = c.ix, y = doc->y, right = c.ix + c.iw;
    bool loaded = !json_is_null(s->pr);
    double additions, deletions;
    bool has_diff = json_num(json_get(s->pr, "additions"), &additions) && json_num(json_get(s->pr, "deletions"), &deletions);
    DiffStatData *ds = NULL; int dsw = 0;
    if (has_diff) { ds = xcalloc(1, sizeof *ds); ds->additions = (int)additions; ds->deletions = (int)deletions; dsw = diffstat_width(doc->cv, ds); right -= dsw + px(16); }
    char count[24];
    const PullSummary *row = board_row(s);
    if (s->run_count) { snprintf(count, sizeof count, "%zu", s->run_count); doc_tab(doc, &x, &y, c.ix, right, h, 0xE8F2, "Sessions", count, s->tab == PR_TAB_SESSIONS, ACT_PR_TAB, PR_TAB_SESSIONS); }
    doc_tab(doc, &x, &y, c.ix, right, h, 0xE7C3, "PR Body", NULL, s->tab == PR_TAB_BODY, ACT_PR_TAB, PR_TAB_BODY);
    if (store_supports(conv_feeds[CONV_COMMENTS].op)) {
        // The messages once they are read; until then the board's count of conversation comments.
        double comments = 0;
        bool has_count = conv_read(s) ? (comments = (double)conv_count(s), true) : row && json_num(json_get(row->raw, "comments"), &comments);
        if (has_count) snprintf(count, sizeof count, "%d", (int)comments);
        doc_tab(doc, &x, &y, c.ix, right, h, 0xE8BD, "Conversation", has_count ? count : NULL, s->tab == PR_TAB_CONVERSATION, ACT_PR_TAB, PR_TAB_CONVERSATION);
    }
    double files;
    bool has_files = json_num(json_get(s->pr, "changedFiles"), &files);
    if (has_files) snprintf(count, sizeof count, "%d", (int)files);
    // The files are a tab here, or GitHub's page when the server cannot list them.
    if (store_supports("pull_files")) doc_tab(doc, &x, &y, c.ix, right, h, 0xE8A5, "Files changed", has_files ? count : NULL, s->tab == PR_TAB_FILES, ACT_PR_TAB, PR_TAB_FILES);
    else if (safe_web_url(json_str(json_get(s->pr, "url")))) doc_tab(doc, &x, &y, c.ix, right, h, 0xE8A5, "Files changed", has_files ? count : NULL, false, ACT_OPEN_URL, 1);
    int commits = pull_commit_count(s);
    if (json_count(json_get(s->pr, "commitList"))) { snprintf(count, sizeof count, "%d", commits); doc_tab(doc, &x, &y, c.ix, right, h, 0xE8EE, "Commits", count, s->tab == PR_TAB_COMMITS, ACT_PR_TAB, PR_TAB_COMMITS); }
    if (loaded) { snprintf(count, sizeof count, "%zu", json_count(json_get(json_get(s->pr, "checks"), "runs"))); doc_tab(doc, &x, &y, c.ix, right, h, 0xE9D5, "Checks", count, s->tab == PR_TAB_CHECKS, ACT_PR_TAB, PR_TAB_CHECKS); }
    if (store_supports("findings")) { snprintf(count, sizeof count, "%zu", json_count(s->findings)); doc_tab(doc, &x, &y, c.ix, right, h, 0xE7C1, "Findings", count, s->tab == PR_TAB_FINDINGS, ACT_PR_TAB, PR_TAB_FINDINGS); }
    if (s->run_url || s->serving) doc_tab(doc, &x, &y, c.ix, right, h, 0xE768, "Run", NULL, s->tab == PR_TAB_RUN, ACT_PR_TAB, PR_TAB_RUN);
    if (ds) { doc->y = y; doc_custom(doc, c.ix + c.iw - dsw, dsw, h, paint_diffstat, ds, free, 0, 0); }
    doc->y = y + h;
    doc_rule(doc, c.x, c.w);
}

/// The author's initial in a circle, where GitHub shows the avatar.
typedef struct { char initial[8]; } AvatarData;
static void paint_avatar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    AvatarData *d = it->data;
    int r = (rc->right - rc->left) / 2;
    fill_circle(cv, rc->left + r, rc->top + r, r, blend(theme.accent, theme.canvas, 0.35));
    RECT t = *rc; draw_text(cv, d->initial, &t, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
/// `.timeline-comment-header`: a tinted strip with `author commented · updated …`, a review's verdict glyph first.
typedef struct { char *author, *when; wchar_t glyph; COLORREF glyph_color; } CommentHeadData;
static void comment_head_free(void *p) { CommentHeadData *d = p; free(d->author); free(d->when); free(d); }
static void paint_comment_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    CommentHeadData *d = it->data;
    COLORREF tint = blend(theme.accent, theme.raise, 0.10);
    RECT top = *rc; fill_round_rect(cv, &top, px(7), tint, tint);
    RECT low = { rc->left, (rc->top + rc->bottom) / 2, rc->right, rc->bottom }; fill_rect(cv, &low, tint);
    draw_line(cv, rc->left, rc->bottom - 1, rc->right, rc->bottom - 1, theme.line);
    int x = rc->left + px(16);
    if (d->glyph) { RECT g = { x, rc->top, x + px(18), rc->bottom }; draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->glyph_color); x += px(24); }
    if (d->author) { int aw = text_width(cv, d->author, FONT_FOOTNOTE_SEMIBOLD); RECT a = { x, rc->top, x + aw + px(2), rc->bottom }; draw_text(cv, d->author, &a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); x += aw + px(5); }
    RECT t = { x, rc->top, rc->right - px(16), rc->bottom };
    draw_text(cv, d->when, &t, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// What GitHub shows of a description: without HTML comments and raw HTML lines (bots' badges and footers), code blocks kept whole.
static char *visible_markdown(const char *body) {
    Str bare; str_init(&bare);
    for (const char *p = body; *p;) {
        if (str_has_prefix(p, "<!--")) { const char *end = strstr(p + 4, "-->"); if (!end) break; p = end + 3; continue; }
        str_appendf(&bare, "%c", *p++);
    }
    Str out; str_init(&out);
    bool fenced = false;
    for (const char *p = bare.data ? bare.data : ""; *p;) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char *line = xstrndup(p, n), *t = str_trim(line);
        if (str_has_prefix(t, "```")) fenced = !fenced;
        if (fenced || str_has_prefix(t, "```") || *t != '<') str_appendf(&out, "%s\n", line);
        free(t); free(line);
        p = e ? e + 1 : p + n;
    }
    str_free(&bare);
    char *result = str_trim(out.data ? out.data : "");
    str_free(&out);
    return result;
}
/// A timeline comment: the avatar where there is room, then a box opened with its header (`h`, which is taken). Returns
/// the box for doc_box_end, with the inner column in `ix`/`iw`. A header with an action opens the comment on GitHub.
static int comment_begin(Doc *doc, Col c, CommentHeadData *h, int action, intptr_t arg, int *ix, int *iw) {
    int x = c.x, w = c.w, top = doc->y;
    if (h->author && *h->author && c.w >= px(520)) {
        AvatarData *a = xcalloc(1, sizeof *a);
        // The first character of the login, which is ASCII on GitHub.
        a->initial[0] = (char)((h->author[0] >= 'a' && h->author[0] <= 'z') ? h->author[0] - 32 : h->author[0]);
        doc_custom(doc, x, px(36), px(36), paint_avatar, a, free, 0, 0);
        doc->y = top; x += px(48); w -= px(48);
    }
    int box = doc_box_begin(doc, x, w, 0, theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int hi = doc_custom(doc, x + 1, w - 2, px(38), paint_comment_head, h, comment_head_free, action, arg);
    doc_item(doc, hi)->hover_fill = false;
    doc_space(doc, px(14));
    *ix = x + px(16); *iw = w - px(32);
    return box;
}
/// The opening comment: the pull request's description as GitHub shows it, read from the server's file list when `pull` leaves it out.
static void layout_description(PullScreen *s, Doc *doc, Col c) {
    const char *body = json_str(json_get(s->pr, "body"));
    if (!body) body = s->body;
    bool loading = !body && !s->body_read && store_supports("pull_description");
    if (!body && !loading) return;
    const char *author = pull_author(s);
    const PullSummary *row = board_row(s);
    CommentHeadData *h = xcalloc(1, sizeof *h);
    h->author = xstrdup(author);
    if (row && row->has_updated) { char *rel = format_relative(row->updated_at); h->when = xstrfmt("%s \xC2\xB7 updated %s", author ? "commented" : "Description", rel); free(rel); }
    else h->when = xstrdup(author ? "commented" : "Description");
    int ix, iw, box = comment_begin(doc, c, h, 0, 0, &ix, &iw);
    if (loading) doc_text(doc, ix, iw, "Loading the description\xE2\x80\xA6", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    else {
        char *trimmed = visible_markdown(body);
        if (*trimmed) doc_markdown(doc, ix, iw, trimmed, FONT_CALLOUT);
        else doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
        free(trimmed);
    }
    doc_box_end(doc, box, px(16));
}

// The Conversation tab: GitHub's timeline of conversation comments, reviews and the line comments they carry.

/// When a comment or review was made, 0 when the server left it out.
static time_t conv_time(const Json *v, const char *field) { time_t t; return board_date_parse(json_str(json_get(v, field)), &t) ? t : 0; }
static bool conv_find(const Json *list, const Json *id, size_t *out) {
    double want, have;
    if (!json_num(id, &want)) return false;
    for (size_t i = 0; i < json_count(list); i++) if (json_num(json_get(json_at(list, i), "id"), &have) && have == want) { *out = i; return true; }
    return false;
}
/// The comment a line comment's thread starts with: each reply points at the comment it answers.
static size_t conv_thread_root(const Json *lines, size_t i) {
    size_t parent;
    for (int hop = 0; hop < 64 && conv_find(lines, json_get(json_at(lines, i), "inReplyTo"), &parent) && parent != i; hop++) i = parent;
    return i;
}
/// The submitted review a thread is shown under, when the server listed it.
static bool conv_thread_review(PullScreen *s, size_t root, size_t *review) {
    const Json *reviews = s->conv[CONV_REVIEWS].items;
    return conv_find(reviews, json_get(json_at(s->conv[CONV_REVIEW_COMMENTS].items, root), "reviewId"), review)
        && !str_ieq(json_str(json_get(json_at(reviews, *review), "state")), "pending");
}
/// A review's verdict as its header words it, with its glyph; NULL for a plain comment review.
static const char *review_verdict(const Json *review, wchar_t *glyph, COLORREF *color) {
    const char *state = json_str(json_get(review, "state"));
    if (str_ieq(state, "approved")) { *glyph = 0xE73E; *color = theme.success; return "approved these changes"; }
    if (str_ieq(state, "changes_requested")) { *glyph = 0xE7BA; *color = theme.danger; return "requested changes"; }
    if (str_ieq(state, "dismissed")) { *glyph = 0xE711; *color = theme.secondary; return "reviewed (dismissed)"; }
    *glyph = 0xE8BD; *color = theme.muted;
    return NULL;
}
static bool has_visible_body(const Json *v) {
    const char *body = json_str(json_get(v, "body"));
    if (!body) return false;
    char *t = visible_markdown(body); bool has = *t != 0; free(t);
    return has;
}
/// A review says something of its own with a verdict or a summary; one that only holds line comments is shown for them.
static bool review_speaks(const Json *review) {
    wchar_t g; COLORREF cc;
    if (str_ieq(json_str(json_get(review, "state")), "pending")) return false;
    return review_verdict(review, &g, &cc) || has_visible_body(review);
}
static bool review_has_threads(PullScreen *s, size_t r) {
    const Json *lines = s->conv[CONV_REVIEW_COMMENTS].items;
    size_t rv;
    for (size_t i = 0; i < json_count(lines); i++) if (conv_thread_root(lines, i) == i && conv_thread_review(s, i, &rv) && rv == r) return true;
    return false;
}
/// The messages in the conversation: every comment, every line comment and each review with a verdict or summary.
static size_t conv_count(PullScreen *s) {
    const Json *reviews = s->conv[CONV_REVIEWS].items;
    size_t n = json_count(s->conv[CONV_COMMENTS].items) + json_count(s->conv[CONV_REVIEW_COMMENTS].items);
    for (size_t i = 0; i < json_count(reviews); i++) if (review_speaks(json_at(reviews, i))) n++;
    return n;
}
/// The timeline, oldest first: comments, reviews, and line comment threads whose review is not listed.
typedef struct { int feed; size_t i; time_t at; } ConvEntry;
static int conv_entry_cmp(const void *a, const void *b) {
    const ConvEntry *x = a, *y = b;
    if (x->at != y->at) return x->at < y->at ? -1 : 1;
    if (x->feed != y->feed) return x->feed - y->feed;
    return x->i < y->i ? -1 : x->i > y->i;
}
static ConvEntry *conv_timeline(PullScreen *s, size_t *n) {
    const Json *comments = s->conv[CONV_COMMENTS].items, *reviews = s->conv[CONV_REVIEWS].items, *lines = s->conv[CONV_REVIEW_COMMENTS].items;
    ConvEntry *e = xcalloc(json_count(comments) + json_count(reviews) + json_count(lines) + 1, sizeof *e);
    *n = 0;
    for (size_t i = 0; i < json_count(comments); i++) e[(*n)++] = (ConvEntry){ CONV_COMMENTS, i, conv_time(json_at(comments, i), "createdAt") };
    for (size_t i = 0; i < json_count(reviews); i++) {
        const Json *r = json_at(reviews, i);
        if (str_ieq(json_str(json_get(r, "state")), "pending")) continue;
        if (review_speaks(r) || review_has_threads(s, i)) e[(*n)++] = (ConvEntry){ CONV_REVIEWS, i, conv_time(r, "submittedAt") };
    }
    size_t rv;
    for (size_t i = 0; i < json_count(lines); i++)
        if (conv_thread_root(lines, i) == i && !conv_thread_review(s, i, &rv)) e[(*n)++] = (ConvEntry){ CONV_REVIEW_COMMENTS, i, conv_time(json_at(lines, i), "createdAt") };
    qsort(e, *n, sizeof *e, conv_entry_cmp);
    return e;
}
static intptr_t conv_arg(int feed, size_t i) { return ((intptr_t)feed << 24) | (intptr_t)i; }
/// The header of a timeline entry: `author did · 3h ago`.
static CommentHeadData *conv_head(const Json *v, const char *did, const char *time_field) {
    CommentHeadData *h = xcalloc(1, sizeof *h);
    const char *author = json_str(json_get(v, "author"));
    h->author = xstrdup(author ? author : "Someone");
    time_t at = conv_time(v, time_field);
    if (at) { char *rel = format_relative(at); h->when = xstrfmt("%s \xC2\xB7 %s", did, rel); free(rel); }
    else h->when = xstrdup(did);
    return h;
}
/// A comment's markdown as GitHub shows it; whether there was any.
static bool layout_comment_body(Doc *doc, int x, int w, const Json *v) {
    const char *body = json_str(json_get(v, "body"));
    char *t = visible_markdown(body ? body : "");
    bool any = *t != 0;
    if (any) doc_markdown(doc, x, w, t, FONT_CALLOUT);
    free(t);
    return any;
}
/// A line comment thread: the file and line, then each comment in it with its author and time.
static void layout_thread(PullScreen *s, Doc *doc, int x, int w, size_t root) {
    const Json *lines = s->conv[CONV_REVIEW_COMMENTS].items, *first = json_at(lines, root);
    int box = doc_box_begin(doc, x, w, 0, theme.canvas, theme.line, px(6));
    doc_item(doc, box)->hover_fill = false;
    doc_space(doc, px(8));
    const char *path = json_str(json_get(first, "path"));
    double line;
    bool has_line = json_num(json_get(first, "line"), &line) || json_num(json_get(first, "originalLine"), &line);
    char *where = has_line ? xstrfmt("%s:%d", path ? path : "", (int)line) : xstrdup(path ? path : "");
    int pi = doc_text(doc, x + px(12), w - px(24), where, FONT_MONO_SMALL, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (safe_web_url(json_str(json_get(first, "url")))) { doc_item(doc, pi)->action = ACT_CONV_URL; doc_item(doc, pi)->arg = conv_arg(CONV_REVIEW_COMMENTS, root); doc_item(doc, pi)->hand = true; }
    free(where);
    doc_space(doc, px(8));
    doc_rule(doc, x, w);
    for (size_t i = 0; i < json_count(lines); i++) {
        if (conv_thread_root(lines, i) != root) continue;
        const Json *cm = json_at(lines, i);
        doc_space(doc, px(10));
        int y = doc->y, h = px(20), ix = x + px(12), iw = w - px(24);
        const char *author = json_str(json_get(cm, "author")); if (!author) author = "Someone";
        int aw = text_width(doc->cv, author, FONT_FOOTNOTE_SEMIBOLD); if (aw > iw) aw = iw;
        RECT a = { ix, y, ix + aw + px(2), y + h }; doc_text_at(doc, &a, author, FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        time_t at = conv_time(cm, "createdAt");
        if (at) { char *rel = format_relative(at); RECT t = { ix + aw + px(8), y, ix + iw, y + h }; doc_text_at(doc, &t, rel, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS); free(rel); }
        doc->y = y + h + px(4);
        layout_comment_body(doc, ix, iw, cm);
    }
    doc_space(doc, px(10));
    doc_box_end(doc, box, 0);
}
static void layout_conversation(PullScreen *s, Doc *doc, Col c) {
    for (int k = 0; k < CONV_FEEDS; k++) if (s->conv[k].error) { doc_notice(doc, c.x, c.w, s->conv[k].error); doc_space(doc, px(12)); break; }
    if (!conv_read(s)) { doc_loading(doc, c.x, c.w, "Loading the conversation\xE2\x80\xA6"); return; }
    size_t n;
    ConvEntry *entries = conv_timeline(s, &n);
    const Json *lines = s->conv[CONV_REVIEW_COMMENTS].items;
    for (size_t e = 0; e < n; e++) {
        const ConvEntry *en = &entries[e];
        const Json *v = json_at(s->conv[en->feed].items, en->i);
        int ix, iw, box, action = safe_web_url(json_str(json_get(v, "url"))) ? ACT_CONV_URL : 0;
        if (e) doc_space(doc, px(16));
        if (en->feed == CONV_COMMENTS) {
            box = comment_begin(doc, c, conv_head(v, "commented", "createdAt"), action, conv_arg(en->feed, en->i), &ix, &iw);
            if (!layout_comment_body(doc, ix, iw, v)) doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
        } else if (en->feed == CONV_REVIEWS) {
            wchar_t glyph; COLORREF color;
            const char *verdict = review_verdict(v, &glyph, &color);
            CommentHeadData *h = conv_head(v, verdict ? verdict : "reviewed", "submittedAt");
            h->glyph = glyph; h->glyph_color = color;
            box = comment_begin(doc, c, h, action, conv_arg(en->feed, en->i), &ix, &iw);
            bool any = layout_comment_body(doc, ix, iw, v);
            size_t rv;
            for (size_t i = 0; i < json_count(lines); i++) {
                if (conv_thread_root(lines, i) != i || !conv_thread_review(s, i, &rv) || rv != en->i) continue;
                if (any) doc_space(doc, px(10));
                layout_thread(s, doc, ix, iw, i);
                any = true;
            }
            if (!any) doc_space(doc, -px(14));
        } else {
            box = comment_begin(doc, c, conv_head(v, "commented on a line", "createdAt"), action, conv_arg(en->feed, en->i), &ix, &iw);
            layout_thread(s, doc, ix, iw, en->i);
        }
        doc_box_end(doc, box, px(14));
    }
    if (!n) doc_text(doc, c.x, c.w, "No comments yet", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    free(entries);
}

static void layout_checks(PullScreen *s, Doc *doc, Col c) {
    int box = col_box(doc, c);
    CountsData *cd = xcalloc(1, sizeof *cd);
    cd->passed = json_int_or(json_get(json_get(s->pr, "checks"), "passed"), 0); cd->failed = json_int_or(json_get(json_get(s->pr, "checks"), "failed"), 0); cd->pending = json_int_or(json_get(json_get(s->pr, "checks"), "pending"), 0);
    doc_custom(doc, c.ix, c.iw, px(22), paint_counts, cd, free, 0, 0);
    const Json *runs = json_get(json_get(s->pr, "checks"), "runs");
    for (size_t i = 0; i < json_count(runs); i++) {
        const Json *check = json_at(runs, i);
        col_gap(doc, c);
        const char *result = json_str(json_get(check, "conclusion")); if (!result) result = json_str(json_get(check, "status")); if (!result) result = "Pending";
        CheckData *d = xcalloc(1, sizeof *d);
        d->glyph = check_glyph(result, &d->color);
        const char *name = json_str(json_get(check, "name")); d->name = xstrdup(name ? name : "Check");
        char *spaced = str_replace(result, "_", " "); d->result = str_capitalized(spaced); free(spaced);
        const char *curl = json_str(json_get(check, "url"));
        doc_custom(doc, c.ix, c.iw, px(22), paint_check, d, check_free, safe_web_url(curl) ? ACT_CHECK_URL : 0, (intptr_t)i);
    }
    if (!json_count(runs)) { col_gap(doc, c); doc_text(doc, c.ix, c.iw, "No checks reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE); }
    doc_box_end(doc, box, px(12));
}

static void layout_commit_list(PullScreen *s, Doc *doc, Col c) {
    const Json *commits = json_get(s->pr, "commitList");
    int box = col_box(doc, c);
    for (size_t i = 0; i < json_count(commits); i++) {
        const Json *cm = json_at(commits, i);
        if (i) col_gap(doc, c);
        const char *sha = json_str(json_get(cm, "sha")); char *short_sha = xstrndup(sha ? sha : "", sha && strlen(sha) > 7 ? 7 : (sha ? strlen(sha) : 0));
        int y = doc->y; int sw = text_width(doc->cv, short_sha, FONT_MONO_SMALL);
        RECT sr = { c.ix + c.iw - sw, y, c.ix + c.iw, y + px(18) }; doc_text_at(doc, &sr, short_sha, FONT_MONO_SMALL, theme.secondary, DT_RIGHT | DT_SINGLELINE);
        const char *message = json_str(json_get(cm, "message"));
        int mi = doc_label(doc, c.ix, c.iw - sw - px(12), 0xE8EE, message ? message : "", FONT_CALLOUT, theme.text);
        if (safe_web_url(json_str(json_get(cm, "url")))) { doc_item(doc, mi)->action = ACT_COMMIT_URL; doc_item(doc, mi)->arg = (intptr_t)i; doc_item(doc, mi)->hand = true; }
        free(short_sha);
    }
    if (!json_count(commits)) doc_text(doc, c.ix, c.iw, "No commits reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    doc_box_end(doc, box, px(12));
}

/// The findings not yet fixed, and those among them the user marked fix.
static size_t findings_unfixed(PullScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < json_count(s->findings); i++) if (!json_bool_is(json_get(json_at(s->findings, i), "fixed"), true)) n++;
    return n;
}
static size_t findings_to_fix(PullScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < json_count(s->findings); i++) {
        const Json *f = json_at(s->findings, i);
        if (!json_bool_is(json_get(f, "fixed"), true) && str_eq(json_str(json_get(f, "decision")), "fix")) n++;
    }
    return n;
}
/// Whether the server's errand list has one; before the list is read, every errand this app knows is taken as offered.
static bool catalog_lists(const Json *catalog, const char *id) {
    if (!json_count(catalog)) return true;
    for (size_t i = 0; i < json_count(catalog); i++) if (str_eq(json_str(json_get(json_at(catalog, i), "id")), id)) return true;
    return false;
}
/// Solve findings is the board's implement-feedback errand started from the findings themselves: offered on an open pull
/// request with findings still to fix, to a token that may start errands on a server that lists this one.
static bool solve_findings_offered(PullScreen *s) {
    if (!store_can_manage() || !store_supports("action") || !catalog_lists(s->catalog, "implement-feedback")) return false;
    if (!str_eq(json_str(json_get(s->pr, "state")), "open") || !json_str(json_get(s->pr, "headRef"))) return false;
    return findings_unfixed(s) > 0;
}

static void layout_findings(PullScreen *s, Doc *doc, Col c) {
    int box = col_box(doc, c);
    if (s->findings_error) { doc_notice(doc, c.ix, c.iw, s->findings_error); doc_space(doc, px(6)); }
    size_t fn = json_count(s->findings);
    for (size_t i = 0; i < fn; i++) {
        const Json *f = json_at(s->findings, i);
        if (i) col_gap(doc, c);
        bool open = finding_open(s, (int)i);
        const char *ft = json_str(json_get(f, "title"));
        int li = doc_label(doc, c.ix, c.iw, open ? 0xE70D : 0xE76C, ft ? ft : "Finding", FONT_CALLOUT, theme.text);
        doc_item(doc, li)->action = ACT_FINDING_TOGGLE; doc_item(doc, li)->arg = (intptr_t)i; doc_item(doc, li)->hand = true;
        Str meta; str_init(&meta);
        if (json_str(json_get(f, "severity"))) str_appendz(&meta, json_str(json_get(f, "severity")));
        bool fixed = json_bool_is(json_get(f, "fixed"), true);
        const char *decision = json_str(json_get(f, "decision"));
        if (fixed) str_appendf(&meta, "%sFixed", meta.len ? " \xC2\xB7 " : "");
        else if (decision) {
            const char *dt = decision; for (int k = 1; k < 4; k++) if (str_eq(decision, decision_ids[k])) dt = decision_titles[k];
            str_appendf(&meta, "%s%s", meta.len ? " \xC2\xB7 " : "", dt);
        }
        if (str_eq(s->deciding, json_str(json_get(f, "key")))) str_appendz(&meta, " \xC2\xB7 saving\xE2\x80\xA6");
        if (meta.len) { doc_space(doc, px(2)); doc_text(doc, c.ix + px(22), c.iw - px(22), meta.data, FONT_CAPTION, fixed ? theme.success : str_eq(decision, "fix") ? theme.warning : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS); }
        str_free(&meta);
        if (open) {
            const char *file = json_str(json_get(f, "file"));
            if (file) { double line; char *loc = json_num(json_get(f, "line"), &line) ? xstrfmt("%s:%d", file, (int)line) : xstrdup(file); doc_space(doc, px(4)); doc_text(doc, c.ix + px(22), c.iw - px(22), loc, FONT_MONO_SMALL, theme.text, DT_WORDBREAK); free(loc); }
            if (safe_web_url(json_str(json_get(f, "url")))) { doc_space(doc, px(4)); int ui = doc_text(doc, c.ix + px(22), c.iw - px(22), "Open finding on GitHub", FONT_CAPTION, theme.accent, DT_SINGLELINE); doc_item(doc, ui)->action = ACT_FINDING_URL; doc_item(doc, ui)->arg = (intptr_t)i; doc_item(doc, ui)->hand = true; }
            if (store_supports("finding_decision") && json_str(json_get(f, "key")) && !fixed) {
                int selected = 0;
                for (int k = 1; k < 4; k++) if (str_eq(decision, decision_ids[k])) selected = k;
                doc_space(doc, px(8));
                doc_segments(doc, c.ix + px(22), c.iw - px(22), decision_titles, 4, selected, ACT_FINDING_DECISION, (intptr_t)(i * 8), s->deciding == NULL);
            }
        }
    }
    if (!fn && !s->findings_error) doc_text(doc, c.ix, c.iw, "No findings reported", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    if (store_supports("finding_decision") && fn) { doc_space(doc, px(8)); doc_text(doc, c.ix, c.iw, "Decisions are saved on the dashboard and mirrored to the pull request\xE2\x80\x99s checklist on GitHub.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
    if (solve_findings_offered(s)) {
        size_t fixes = findings_to_fix(s);
        doc_space(doc, px(12));
        char *label = s->busy ? xstrdup("Starting\xE2\x80\xA6") : fixes ? xstrfmt("Solve findings \xC2\xB7 %zu to fix", fixes) : xstrdup("Solve findings");
        ButtonSpec b = { 0, label, BUTTON_PROMINENT, ACT_SOLVE_FINDINGS, 0, !s->busy && !s->uncertain && !s->deciding };
        doc_button_row(doc, c.ix, c.iw, &b, 1);
        free(label);
        doc_space(doc, px(6));
        doc_text(doc, c.ix, c.iw, fixes ? "Starts a paid session that addresses the findings marked Fix, pushes the fixes and has them reviewed again."
                                        : "Starts a paid session that addresses the open findings, pushes the fixes and has them reviewed again. Mark what to fix first to narrow it down.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
    doc_box_end(doc, box, px(12));
}

/// The trash button at the right of a session row: muted, red with a tint under the pointer, an ellipsis while deleting.
static void paint_delete_run(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool deleting = it->data != NULL;
    bool hovered = it->action && doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(6), blend(theme.danger, theme.elevated, 0.14), blend(theme.danger, theme.elevated, 0.14));
    RECT r = *rc;
    if (deleting) draw_text(cv, "\xE2\x80\xA6", &r, FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else draw_glyph(cv, 0xE74D, rc, FONT_ICON_SMALL, hovered ? theme.danger : (it->action ? theme.muted : theme.line_strong));
}

static void layout_runs(PullScreen *s, Doc *doc, Col c) {
    if (s->run_error) { doc_notice(doc, c.x, c.w, s->run_error); doc_space(doc, px(8)); }
    if (!s->run_count) { doc_text(doc, c.x, c.w, "No conversations on this pull request", FONT_CALLOUT, theme.secondary, DT_SINGLELINE); return; }
    bool deletable = store_supports("delete");
    int bw = px(28);
    for (size_t i = 0; i < s->run_count; i++) {
        int box = doc_session_row(doc, c.x, c.w, &s->runs[i], ACT_OPEN_RUN, (intptr_t)i, false, theme.elevated, deletable ? bw + px(8) : 0);
        if (deletable) {
            // Laid after the row, so it is the topmost item there and takes the click instead of the row.
            RECT row = doc_item(doc, box)->rc;
            int mid = (row.top + row.bottom) / 2;
            RECT br = { row.right - px(10) - bw, mid - bw / 2, row.right - px(10), mid + bw / 2 };
            int di = doc_add(doc, &br, paint_delete_run);
            Item *it = doc_item(doc, di);
            bool mine = s->deleting_run && str_eq(s->deleting_run, session_id(&s->runs[i]));
            it->data = mine ? (void *)1 : NULL;   // a marker only, never freed
            if (!s->deleting_run) { it->action = ACT_DELETE_RUN; it->arg = (intptr_t)i; it->hand = true; }
        }
        doc_space(doc, px(6));
    }
}

static void delete_run_done(void *owner, Request *req) {
    PullScreen *s = owner;
    const char *id = json_str(json_get(req->args, "sessionId"));
    if (req->ok) {
        set_string(&s->run_error, NULL);
        // The sidebar drops the row now instead of at its next poll, and so does this list.
        sessions_forget(s->project.repo, id);
        for (size_t i = 0; i < s->run_count; i++) {
            if (!str_eq(session_id(&s->runs[i]), id)) continue;
            session_free(&s->runs[i]);
            memmove(&s->runs[i], &s->runs[i + 1], (s->run_count - i - 1) * sizeof *s->runs);
            s->run_count--;
            break;
        }
    } else { request_error_into(&s->run_error, req); }
    set_string(&s->deleting_run, NULL);
    // A list read while the delete was in flight may still hold the row: read it again.
    if (store_supports("sessions")) {
        request_cancel(&s->req_sessions);
        Json *a = json_object(); json_set_str(a, "repo", s->project.repo);
        store_call("sessions", a, 0, s, sessions_done_pull, TAG_SESSIONS, &s->req_sessions);
    }
    pane_relayout(s->base.pane);
}

/// The main column: what the selected tab holds.
static void layout_main(PullScreen *s, Doc *doc, Col c) {
    if (json_is_null(s->pr)) {
        if (s->tab == PR_TAB_SESSIONS) layout_runs(s, doc, c);
        else if (!s->error) doc_loading(doc, c.x, c.w, NULL);
        return;
    }
    switch (s->tab) {
    case PR_TAB_FILES: pull_files_layout(s->files, doc, c.x, c.w); break;
    case PR_TAB_CONVERSATION: layout_conversation(s, doc, c); break;
    case PR_TAB_COMMITS: layout_commit_list(s, doc, c); break;
    case PR_TAB_CHECKS: layout_checks(s, doc, c); break;
    case PR_TAB_FINDINGS: layout_findings(s, doc, c); break;
    case PR_TAB_SESSIONS: layout_runs(s, doc, c); break;
    default:
        layout_description(s, doc, c);
        break;
    }
}

/// `.discussion-sidebar-item`: a small heading over its contents, a rule between items.
static void side_heading(Doc *doc, Col c, int *count, const char *title) {
    if ((*count)++) { doc_space(doc, px(14)); doc_rule(doc, c.x, c.w); }
    doc_space(doc, px(14));
    doc_text(doc, c.x, c.w, title, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(8));
}
/// The errands as the sidebar's first item: one full-width button per action, the one the pull request's state asks for filled.
static void side_actions(PullScreen *s, Doc *doc, Col c, int *count) {
    if (!s->action_count) return;
    const PullSummary *row = board_row(s);
    side_heading(doc, c, count, s->busy ? "Actions \xC2\xB7 starting\xE2\x80\xA6" : "Actions");
    bool enabled = !s->busy && !s->uncertain;
    for (size_t i = 0; i < s->action_count; i++) {
        const BoardAction *a = &s->actions[i];
        bool suggested = row && str_eq(row->recommended, a->id);
        char *label = xstrfmt("%s %s", action_icon(a->id), a->label);
        if (i) doc_space(doc, px(6));
        doc_button(doc, c.x, c.w, label, suggested ? BUTTON_PROMINENT : str_eq(a->id, "delete-self-comments") ? BUTTON_DESTRUCTIVE : BUTTON_BORDERED, ACT_START_ACTION, (intptr_t)i, enabled);
        free(label);
    }
    doc_space(doc, px(8));
    doc_text(doc, c.x, c.w, "Uses the provider and model configured for this project. These actions run paid agents and may write to GitHub.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
}
/// The checks as counts only: passed, pending and failed; the row opens the Checks tab for the individual runs.
static void side_checks(PullScreen *s, Doc *doc, Col c, int *count) {
    if (json_is_null(s->pr)) return;
    const Json *checks = json_get(s->pr, "checks");
    side_heading(doc, c, count, "Checks");
    if (!json_count(json_get(checks, "runs"))) { doc_text(doc, c.x, c.w, "No checks reported", FONT_CAPTION, theme.secondary, DT_SINGLELINE); return; }
    CountsData *cd = xcalloc(1, sizeof *cd);
    cd->passed = json_int_or(json_get(checks, "passed"), 0); cd->pending = json_int_or(json_get(checks, "pending"), 0); cd->failed = json_int_or(json_get(checks, "failed"), 0);
    int i = doc_custom(doc, c.x, c.w, px(22), paint_counts, cd, free, s->tab == PR_TAB_CHECKS ? 0 : ACT_PR_TAB, PR_TAB_CHECKS);
    doc_item(doc, i)->hover_fill = false; doc_item(doc, i)->hand = s->tab != PR_TAB_CHECKS;
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
    const PullSummary *row = board_row(s);
    side_heading(doc, c, count, "Reviewers");
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
    const PullSummary *row = board_row(s);
    if (!row) return;
    side_heading(doc, c, count, "Assignees");
    for (size_t i = 0; i < row->assignee_count; i++) { if (i) doc_space(doc, px(4)); doc_text(doc, c.x, c.w, row->assignees[i], FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS); }
    if (!row->assignee_count) doc_text(doc, c.x, c.w, "No one", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
}
static void side_milestone(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = board_row(s);
    const char *milestone = row ? json_str(json_get(row->raw, "milestone")) : NULL;
    if (!milestone) return;
    side_heading(doc, c, count, "Milestone");
    doc_text(doc, c.x, c.w, milestone, FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
}
static void side_labels(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = board_row(s);
    if (!row) return;
    side_heading(doc, c, count, "Labels");
    if (row->label_count) doc_label_chips(doc, c.x, c.w, row->labels, row->label_count, theme.canvas);
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
}
static void side_development(PullScreen *s, Doc *doc, Col c, int *count) {
    const PullSummary *row = board_row(s);
    const BoardLink *issues = row && row->issue_count ? row->issues : NULL; size_t issue_count = row ? row->issue_count : 0;
    BoardLink *parsed = NULL; size_t parsed_count = 0;
    if (!issues) {
        const Json *arr = json_get(s->pr, "issues");
        parsed = xcalloc(json_count(arr) ? json_count(arr) : 1, sizeof *parsed);
        for (size_t i = 0; i < json_count(arr); i++) if (board_link_parse(json_at(arr, i), &parsed[parsed_count])) parsed_count++;
        issues = parsed; issue_count = parsed_count;
    }
    if (issue_count) {
        side_heading(doc, c, count, "Development");
        doc_text(doc, c.x, c.w, "Successfully merging this pull request may close these issues.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_space(doc, px(6));
        for (size_t i = 0; i < issue_count; i++) { if (i) doc_space(doc, px(2)); doc_linked_row(doc, c.x, c.w, &issues[i], s->project.repo, safe_web_url(issues[i].url) ? ACT_ISSUE_URL : 0, (intptr_t)i); }
    }
    for (size_t i = 0; i < parsed_count; i++) board_link_free(&parsed[i]);
    free(parsed);
}
/// The sidebar: only what the server reports; GitHub's projects and notifications are not part of it.
static void layout_sidebar(PullScreen *s, Doc *doc, Col c) {
    int count = 0;
    side_actions(s, doc, c, &count);
    side_checks(s, doc, c, &count);
    side_reviewers(s, doc, c, &count);
    side_assignees(s, doc, c, &count);
    side_labels(s, doc, c, &count);
    side_milestone(s, doc, c, &count);
    side_development(s, doc, c, &count);
}

/// The Run tab: the browser's area, down to the bottom of the pane, with what is happening written under it until the
/// page is up.
static void layout_run(PullScreen *s, Doc *doc, int w) {
    RECT view = pane_content_rect(s->base.pane);
    int top = doc->y, h = (view.bottom - view.top) - top - px(12);
    if (h < px(320)) h = px(320);
    SetRect(&s->web_rc, 0, top, w, top + h);
    const char *error = s->web ? webview_error(s->web) : NULL;
    if (!s->web || !webview_ready(s->web)) {
        const char *text = error ? error : s->run_url ? "Starting the browser\xE2\x80\xA6" : "Preparing a workspace for this pull request and serving it\xE2\x80\xA6";
        doc_text(doc, px(4), w - px(8), text, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
        if (error && s->run_url) {
            doc_space(doc, px(8));
            int i = doc_text(doc, px(4), w - px(8), "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
            doc_item(doc, i)->action = ACT_WEB_BROWSER; doc_item(doc, i)->hand = true;
        }
    }
    doc->y = top + h;
}
static void web_changed(void *ctx) {
    PullScreen *s = ctx;
    if (s->base.pane && pane_top(s->base.pane) == &s->base) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
/// Opens the served address in the Run tab, starting the browser again when the address changed.
static void show_run(PullScreen *s, const char *url) {
    if (!str_eq(s->run_url, url)) { webview_free(s->web); s->web = NULL; set_string(&s->run_url, url); }
    s->tab = PR_TAB_RUN;
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
static void pull_place(Screen *base, const RECT *content, int scroll_y) {
    PullScreen *s = (PullScreen *)base;
    bool on = s->shown && s->tab == PR_TAB_RUN && s->run_url;
    // The browser is a child of the pane, so it starts once the tab is first shown in one.
    if (on && !s->web) s->web = webview_new(pane_hwnd(base->pane), s->run_url, web_changed, s);
    if (!s->web) return;
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(base->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(base->pane)) / 2;
        RECT r = { content->left + m + s->web_rc.left, content->top + s->web_rc.top - scroll_y, content->left + m + s->web_rc.right, content->top + s->web_rc.bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else webview_set_bounds(s->web, &visible);
    }
    webview_show(s->web, on);
}

static void pull_layout(Screen *base, Doc *doc) {
    PullScreen *s = (PullScreen *)base;
    int w = doc->width;
    doc_space(doc, px(14));
    if (s->error) { doc_notice(doc, px(4), w - px(8), s->error); doc_space(doc, px(10)); }
    if (s->write_error) {
        doc_notice(doc, px(4), w - px(8), s->write_error);
        if (s->uncertain) { doc_space(doc, px(4)); doc_text(doc, px(4), w - px(8), "The request may have completed. Refresh (F5) and look for its conversation in the Sessions tab before starting another agent.", FONT_CAPTION, theme.secondary, DT_WORDBREAK); }
        doc_space(doc, px(10));
    }
    Col head = { 0, w, px(4), w - px(8) };
    layout_header(s, doc, head);
    doc_space(doc, px(12));
    layout_tabs(s, doc, head);
    doc_space(doc, px(18));
    if (s->tab == PR_TAB_RUN) {
        layout_run(s, doc, w);
        return;
    } else if (s->tab == PR_TAB_FILES && !json_is_null(s->pr)) {
        // The files take the whole width: GitHub's Files changed tab has no sidebar.
        layout_main(s, doc, col_make(0, w));
    } else if (w >= px(880)) {
        // Wide: the conversation on the left, GitHub's sidebar on the right.
        int side_w = w * 26 / 100, gap = px(28);
        if (side_w < px(240)) side_w = px(240);
        if (side_w > px(320)) side_w = px(320);
        Col main = col_make(0, w - side_w - gap), side = { w - side_w, side_w, w - side_w, side_w };
        int top = doc->y;
        layout_main(s, doc, main);
        int main_bottom = doc->y;
        doc->y = top - px(14);
        layout_sidebar(s, doc, side);
        if (doc->y < main_bottom) doc->y = main_bottom;
    } else {
        layout_main(s, doc, col_make(0, w));
        doc_space(doc, px(6));
        Col side = { px(4), w - px(8), px(4), w - px(8) };
        layout_sidebar(s, doc, side);
    }
    doc_space(doc, px(16));
}

static void pull_header(Screen *base, HeaderInfo *info) {
    PullScreen *s = (PullScreen *)base;
    snprintf(info->title, sizeof info->title, "Pull request");
    snprintf(info->subtitle, sizeof info->subtitle, "%s #%d", s->project.repo, s->number);
    if (s->tab != PR_TAB_RUN || !s->run_url) return;
    const char *url = s->web && webview_url(s->web) ? webview_url(s->web) : s->run_url;
    snprintf(info->subtitle, sizeof info->subtitle, "%s #%d \xC2\xB7 %s", s->project.repo, s->number, url);
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_WEB_RELOAD; r->enabled = s->web && webview_ready(s->web); r->tip = "Reload the page";
    HeaderButton *o = &info->buttons[info->button_count++];
    o->glyph = 0xE8A7; o->action = ACT_WEB_BROWSER; o->enabled = true; o->tip = "Open in your browser";
}

static void start_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        set_string(&s->write_error, NULL);
        const char *url = json_str(json_get(req->result, "url"));
        if (s->serving && safe_web_url(url)) show_run(s, url);
        // The pull request stays shown; the new session joins its runs.
        if (store_supports("sessions")) { request_cancel(&s->req_sessions); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (!api_error_is_refusal(&req->error)) s->uncertain = true;
    }
    // A Run that failed before serving anything leaves no tab to show.
    if (s->serving && !s->run_url && s->tab == PR_TAB_RUN) s->tab = PR_TAB_BODY;
    s->serving = false;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void start_action(PullScreen *s, const BoardAction *action, const char *input) {
    const char *branch = json_str(json_get(s->pr, "headRef"));
    if (s->busy || s->uncertain || !branch) return;
    s->busy = true; s->serving = str_eq(action->id, "run");
    if (s->serving) { s->tab = PR_TAB_RUN; pane_header_changed(s->base.pane); }
    char *op = board_action_operation(action);
    Json *args = board_action_arguments(action, s->project.repo, s->number, branch, input);
    store_call(op, args, board_action_timeout_ms(action), s, start_done, TAG_START, &s->req_start);
    free(op);
    pane_relayout(s->base.pane);
}
static void merge_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->merging = false;
    if (!req->ok) {
        char *t = request_error_text(req);
        if (api_error_is_refusal(&req->error)) set_string(&s->merge_error, t);
        else { char *m = xstrfmt("%s The merge may still have completed; check the state above before trying again.", t); set_string(&s->merge_error, m); free(m); }
        free(t);
    } else set_string(&s->merge_error, NULL);
    request_cancel(&s->req_pull); pull_load(s);
    pane_relayout(s->base.pane);
}
static void merge(PullScreen *s, const char *method) {
    const char *head = json_str(json_get(s->pr, "headSha")), *base_ref = json_str(json_get(s->pr, "baseRef"));
    if (s->merging || !head || !base_ref) return;
    s->merging = true; set_string(&s->merge_error, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref); json_set_str(args, "method", method);
    store_call("merge_pull", args, 0, s, merge_done, TAG_MERGE, &s->req_merge);
    pane_relayout(s->base.pane);
}
static void decide_done(void *owner, Request *req) {
    PullScreen *s = owner;
    set_string(&s->deciding, NULL);
    if (req->ok) { json_free(s->findings); s->findings = json_clone(json_get(req->result, "findings")); set_string(&s->findings_error, NULL); }
    else { request_error_into(&s->findings_error, req); }
    pane_relayout(s->base.pane);
}
static void decide(PullScreen *s, const char *key, const char *decision) {
    if (s->deciding) return;
    set_string(&s->deciding, key);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number); json_set_str(args, "key", key);
    json_object_set(args, "decision", json_string_or_null(decision));
    store_call("finding_decision", args, 0, s, decide_done, TAG_DECIDE, &s->req_decide);
    pane_relayout(s->base.pane);
}

static void pull_refresh(Screen *base);
static void pull_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    PullScreen *s = (PullScreen *)base;
    switch (action) {
    case ACT_OPEN_URL: {
        const char *url = json_str(json_get(s->pr, "url"));
        if (arg == 1) { char *files = xstrfmt("%s/files", url); open_web_url(files); free(files); } else open_web_url(url);
        break;
    }
    case ACT_STACK_ITEM: app_push_detail(pull_detail_screen_new(&s->project, (int)arg, s->has_stack ? &s->stack : NULL, NULL)); break;
    case ACT_STACK_TOGGLE: s->stack_open = !s->stack_open; pane_relayout(base->pane); break;
    case ACT_PR_TAB:
        s->tab = (int)arg;
        if (s->tab == PR_TAB_FILES) pull_files_load(s->files);
        if (s->tab == PR_TAB_CONVERSATION) conv_load(s);
        pane_relayout(base->pane); pane_header_changed(base->pane);
        break;
    case ACT_WEB_RELOAD: if (s->web) webview_reload(s->web); break;
    case ACT_WEB_BROWSER: open_web_url(s->web && webview_url(s->web) ? webview_url(s->web) : s->run_url); break;
    case ACT_CONV_URL: {
        const Json *feed = s->conv[arg >> 24].items;
        open_web_url(json_str(json_get(json_at(feed, (size_t)(arg & 0xFFFFFF)), "url")));
        break;
    }
    case ACT_FINDING_TOGGLE: {
        bool open = finding_open(s, (int)arg);
        if (open) { for (size_t k = 0; k < s->open_finding_count; k++) if (s->open_findings[k] == (int)arg) { s->open_findings[k] = s->open_findings[--s->open_finding_count]; break; } }
        else { s->open_findings = xrealloc(s->open_findings, (s->open_finding_count + 1) * sizeof *s->open_findings); s->open_findings[s->open_finding_count++] = (int)arg; }
        pane_relayout(base->pane);
        break;
    }
    case ACT_FINDING_DECISION: {
        size_t index = (size_t)(arg / 8); int d = (int)(arg % 8);
        const char *key = json_str(json_get(json_at(s->findings, index), "key"));
        if (key && d >= 0 && d < 4) decide(s, key, d == 0 ? NULL : decision_ids[d]);
        break;
    }
    case ACT_FINDING_URL: open_web_url(json_str(json_get(json_at(s->findings, (size_t)arg), "url"))); break;
    case ACT_CHECK_URL: open_web_url(json_str(json_get(json_at(json_get(json_get(s->pr, "checks"), "runs"), (size_t)arg), "url"))); break;
    case ACT_COMMIT_URL: open_web_url(json_str(json_get(json_at(json_get(s->pr, "commitList"), (size_t)arg), "url"))); break;
    case ACT_ISSUE_URL: {
        const PullSummary *row = board_row(s);
        if (row && (size_t)arg < row->issue_count) open_web_url(row->issues[arg].url);
        else open_web_url(json_str(json_get(json_at(json_get(s->pr, "issues"), (size_t)arg), "url")));
        break;
    }
    case ACT_MERGE: merge(s, "squash"); break;
    case ACT_RELOAD: pull_refresh(base); break;
    case ACT_START_ACTION: {
        if ((size_t)arg >= s->action_count) break;
        // The list may be rebuilt while the prompt is up, so the errand is copied first.
        BoardAction a; board_action_copy(&a, &s->actions[arg]);
        char *input = NULL;
        s->dialog_open = true;
        bool ok = action_prompt(&a, s->number, &input);
        s->dialog_open = false;
        if (ok) start_action(s, &a, input);
        free(input); board_action_free(&a);
        break;
    }
    case ACT_SOLVE_FINDINGS: {
        if (!solve_findings_offered(s) || s->busy || s->uncertain || s->deciding) break;
        // The errand as the server words it when it is on offer, else as this app knows it.
        const BoardAction *src = NULL;
        for (size_t i = 0; i < s->action_count && !src; i++) if (str_eq(s->actions[i].id, "implement-feedback")) src = &s->actions[i];
        if (!src) { size_t kn; const BoardAction *known = board_actions_known(&kn); for (size_t i = 0; i < kn && !src; i++) if (str_eq(known[i].id, "implement-feedback")) src = &known[i]; }
        if (!src) break;
        BoardAction a; board_action_copy(&a, src);
        size_t fixes = findings_to_fix(s), open = findings_unfixed(s);
        char *title = fixes ? xstrfmt("Start a paid fix session for %zu finding%s?", fixes, fixes == 1 ? "" : "s")
                            : xstrfmt("Start a paid fix session for %zu open finding%s?", open, open == 1 ? "" : "s");
        char *message = xstrfmt("The agent addresses the findings on PR #%d, pushes the fixes to its branch and has them reviewed again. Uses the provider and model configured for this project.", s->number);
        s->dialog_open = true;
        bool ok = app_confirm(title, message, "Solve findings", false);
        s->dialog_open = false;
        free(title); free(message);
        if (ok) start_action(s, &a, NULL);
        board_action_free(&a);
        break;
    }
    case ACT_OPEN_RUN: if ((size_t)arg < s->run_count) app_push_detail(conversation_screen_new(&s->runs[arg])); break;
    case ACT_DELETE_RUN: {
        if ((size_t)arg >= s->run_count || s->deleting_run || !store_supports("delete")) break;
        char *id = xstrdup(session_id(&s->runs[arg]));
        char *message = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D", session_display_title(&s->runs[arg]));
        s->dialog_open = true;
        bool ok = app_confirm("Permanently delete this conversation and its transcript?", message, "Delete", true);
        s->dialog_open = false;
        free(message);
        // The list may have been read again while the dialog was open: delete by id, not by row.
        if (ok && !s->deleting_run) {
            set_string(&s->deleting_run, id); set_string(&s->run_error, NULL);
            Json *args = json_object(); json_set_str(args, "sessionId", id);
            store_call("delete", args, 0, s, delete_run_done, TAG_DELETE_RUN, &s->req_delete_run);
            pane_relayout(s->base.pane);
        }
        free(id);
        break;
    }
    default: pull_files_action(s->files, action, arg); break;
    }
}
static void pull_timer(Screen *base, UINT id) {
    PullScreen *s = (PullScreen *)base;
    if (pull_files_timer(s->files, id)) return;
    if (poller_fired(&s->poller, id)) {
        bool enabled = !s->busy && !s->merging && !s->deciding && !s->dialog_open;
        if (enabled) pull_load(s); else poller_finished(&s->poller, false, -1);
    }
}
static void pull_visible(Screen *base, bool shown) {
    PullScreen *s = (PullScreen *)base;
    s->shown = shown;
    if (s->web && !shown) webview_show(s->web, false);
    if (shown) { poller_start(&s->poller, base->pane, TIMER_POLL, 30000); if (s->tab == PR_TAB_FILES) pull_files_load(s->files); if (s->tab == PR_TAB_CONVERSATION) conv_load(s); }
    else { poller_stop(&s->poller); pull_files_cancel(s->files); conv_cancel(s); request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_rows); request_cancel(&s->req_actions); request_cancel(&s->req_sessions); request_cancel(&s->req_body); }
}
static void pull_refresh(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    // F5 on the Run tab reloads the page, as in a browser.
    if (s->tab == PR_TAB_RUN && s->web) { webview_reload(s->web); return; }
    // Refreshing is how an uncertain start is checked: its conversation is listed in the Sessions tab if it began.
    s->uncertain = false; set_string(&s->write_error, NULL);
    request_cancel(&s->req_body); s->body_read = false;
    request_cancel(&s->req_pull); pull_load(s);
    if (pull_files_started(s->files)) pull_files_refresh(s->files);
    pane_relayout(base->pane);
}
static void pull_activated(Screen *base, bool active) { if (active) { PullScreen *s = (PullScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 30000); } }
static const ScreenVTable pull_vt = {
    .destroy = pull_destroy, .layout = pull_layout, .header = pull_header, .action = pull_action, .timer = pull_timer,
    .visible = pull_visible, .refresh = pull_refresh, .activated = pull_activated, .place = pull_place,
};
Screen *pull_detail_screen_new(const Project *project, int number, const StackPosition *stack, const PullSummary *summary) {
    PullScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pull_vt; s->base.id = xstrfmt("pull:%s#%d", project->repo, number);
    project_copy(&s->project, project); s->number = number;
    if (stack) { stack_position_copy(&s->stack, stack); s->has_stack = true; }
    if (summary) { pull_summary_copy(&s->summary, summary); s->has_summary = true; }
    s->pr = json_null(); s->catalog = json_array(); s->findings = json_array();
    s->files = pull_files_new(project, number, &s->base, ACT_FILES_BASE, TIMER_FILES_PAGE);
    return &s->base;
}

static void board_open_served(PullsScreen *s, const Json *result) {
    const char *url = json_str(json_get(result, "url"));
    // Only while the board is still in front: a Run that took minutes must not pull the user away.
    if (!safe_web_url(url) || !s->base.pane || pane_top(s->base.pane) != &s->base) return;
    const PullSummary *pull = NULL;
    for (size_t i = 0; i < s->pull_count; i++) if (s->pulls[i].number == s->starting_number) pull = &s->pulls[i];
    StackPosition stack; bool has_stack = pull && stack_position_parse(json_get(pull->raw, "stack"), json_get(s->board, "stacks"), &stack);
    Screen *screen = pull_detail_screen_new(&s->project, s->starting_number, has_stack ? &stack : NULL, pull);
    if (has_stack) stack_position_free(&stack);
    show_run((PullScreen *)screen, url);
    app_push_detail(screen);
}

// MARK: - Issue

enum { ACT_ISSUE_OPEN = 1200, ACT_ISSUE_PARENT, ACT_ISSUE_PULL, ACT_ISSUE_START, ACT_ISSUE_CHECKED };

typedef struct {
    Screen base;
    Project project; IssueSummary issue;
    bool busy, uncertain;
    char *write_error;
    Request *req;
} IssueScreen;

static void issue_destroy(Screen *base) {
    IssueScreen *s = (IssueScreen *)base;
    request_cancel(&s->req);
    project_free(&s->project); issue_summary_free(&s->issue); free(s->write_error);
    screen_release(base);
}
static void issue_layout(Screen *base, Doc *doc) {
    IssueScreen *s = (IssueScreen *)base;
    int w = doc->width, ix = px(12), iw = w - px(24);
    const IssueSummary *issue = &s->issue;
    doc_space(doc, px(10));
    if (s->write_error) {
        int b = section_box(doc, w);
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
    int box = section_box(doc, w);
    doc_text(doc, ix, iw, issue->title, FONT_TITLE3, theme.text, DT_WORDBREAK);
    if (issue_is_epic(issue)) { row_gap(doc, w); char *t = xstrfmt("%d/%d done", issue->sub_issues_done, issue->sub_issues); doc_labeled(doc, ix, iw, "Sub-issues", t, theme.secondary); free(t); }
    if (issue->label_count) { row_gap(doc, w); doc_label_chips(doc, ix, iw, issue->labels, issue->label_count, theme.elevated); }
    if (issue->author) { row_gap(doc, w); char *a = xstrfmt("@%s", issue->author); doc_labeled(doc, ix, iw, "Reported by", a, theme.text); free(a); }
    row_gap(doc, w);
    { char *a = issue->assignee_count ? people(issue->assignees, issue->assignee_count, 4) : xstrdup("Nobody"); doc_labeled(doc, ix, iw, "Assigned", a, theme.text); free(a); }
    if (issue->milestone) { row_gap(doc, w); doc_labeled(doc, ix, iw, "Milestone", issue->milestone, theme.text); }
    if (issue->comments > 0) { row_gap(doc, w); char *c = xstrfmt("%d", issue->comments); doc_labeled(doc, ix, iw, "Comments", c, theme.text); free(c); }
    if (issue->has_updated) { row_gap(doc, w); char *rel = format_relative(issue->updated_at); doc_labeled(doc, ix, iw, "Updated", rel, theme.secondary); free(rel); }
    if (safe_web_url(issue->url)) { row_gap(doc, w); int li = doc_label(doc, ix, iw, 0xE8A7, "Open on GitHub", FONT_CALLOUT, theme.accent); doc_item(doc, li)->action = ACT_ISSUE_OPEN; doc_item(doc, li)->hand = true; }
    doc_box_end(doc, box, px(12));
    if (issue->has_parent) {
        doc_section(doc, 0, w, "Part of");
        box = section_box(doc, w);
        doc_linked_row(doc, ix, iw, &issue->parent, s->project.repo, safe_web_url(issue->parent.url) ? ACT_ISSUE_PARENT : 0, 0);
        doc_box_end(doc, box, px(12));
    }
    doc_section(doc, 0, w, "Pull requests");
    box = section_box(doc, w);
    for (size_t i = 0; i < issue->pull_count; i++) {
        if (i) row_gap(doc, w);
        const BoardLink *pull = &issue->pulls[i];
        bool foreign = board_link_is_foreign(pull, s->project.repo) || !store_supports("pull");
        doc_linked_row(doc, ix, iw, pull, s->project.repo, foreign ? (safe_web_url(pull->url) ? ACT_ISSUE_PULL : 0) : ACT_ISSUE_PULL, (intptr_t)i);
    }
    if (!issue->pull_count) doc_text(doc, ix, iw, "No open pull request closes this issue yet", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
    doc_box_end(doc, box, px(12));
    if (store_supports("start_session")) {
        doc_space(doc, px(14));
        box = section_box(doc, w);
        if (issue_is_epic(issue)) {
            doc_text(doc, ix, iw, "An epic is worked by an orchestrator, one sub-issue at a time. Start it from the web dashboard, where its models are picked, or start one of its sub-issues here.", FONT_FOOTNOTE, theme.secondary, DT_WORDBREAK);
        } else {
            doc_button(doc, ix, iw, s->busy ? "Starting\xE2\x80\xA6" : "Start a session on this issue", BUTTON_PROMINENT, ACT_ISSUE_START, 0, !s->busy && !s->uncertain);
            doc_space(doc, px(8));
            doc_text(doc, ix, iw, issue->pull_count ? "A pull request is already answering this issue. A second session is a paid agent working on the same thing."
                                                    : "The session reads the issue, implements it on a branch of its own and opens a pull request closing it. It runs a paid agent on this project\xE2\x80\x99s configured model.",
                     FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        }
        doc_box_end(doc, box, px(12));
    }
    doc_space(doc, px(16));
}
static void issue_header(Screen *base, HeaderInfo *info) { IssueScreen *s = (IssueScreen *)base; snprintf(info->title, sizeof info->title, "#%d", s->issue.number); snprintf(info->subtitle, sizeof info->subtitle, "%s", s->project.repo); }
static void issue_start_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        Session started;
        if (session_parse(json_get(req->result, "session"), &started)) { pane_relayout(s->base.pane); app_push_detail(conversation_screen_new(&started)); session_free(&started); return; }
    } else {
        request_error_into(&s->write_error, req);
        if (!api_error_is_refusal(&req->error)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}
static void issue_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    IssueScreen *s = (IssueScreen *)base;
    switch (action) {
    case ACT_ISSUE_OPEN: open_web_url(s->issue.url); break;
    case ACT_ISSUE_PARENT: open_web_url(s->issue.parent.url); break;
    case ACT_ISSUE_PULL: {
        if ((size_t)arg >= s->issue.pull_count) break;
        const BoardLink *pull = &s->issue.pulls[arg];
        if (board_link_is_foreign(pull, s->project.repo) || !store_supports("pull")) open_web_url(pull->url);
        else app_push_detail(pull_detail_screen_new(&s->project, pull->number, NULL, NULL));
        break;
    }
    case ACT_ISSUE_CHECKED: s->uncertain = false; set_string(&s->write_error, NULL); pane_relayout(base->pane); break;
    case ACT_ISSUE_START: {
        if (s->busy || s->uncertain) break;
        char *title = xstrfmt("Start a paid session on issue #%d?", s->issue.number);
        bool ok = app_confirm(title, NULL, "Start session", false);
        free(title);
        if (!ok) break;
        s->busy = true;
        char *prompt = issue_prompt(&s->issue, s->project.repo);
        Json *args = json_object();
        json_set_str(args, "repo", s->project.repo); json_set_str(args, "prompt", prompt); json_set_str(args, "activity", "issue");
        free(prompt);
        store_call("start_session", args, 0, s, issue_start_done, 0, &s->req);
        pane_relayout(base->pane);
        break;
    }
    }
}
static const ScreenVTable issue_vt = { .destroy = issue_destroy, .layout = issue_layout, .header = issue_header, .action = issue_action };
Screen *issue_detail_screen_new(const Project *project, const IssueSummary *issue) {
    IssueScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &issue_vt; s->base.id = xstrfmt("issue:%s#%d", project->repo, issue->number);
    project_copy(&s->project, project);
    // A deep copy through JSON keeps the row's own parsing in one place.
    Json *j = json_object();
    json_set_num(j, "number", issue->number); json_set_str(j, "title", issue->title); json_set_str(j, "url", issue->url); json_set_str(j, "author", issue->author);
    json_set_str(j, "milestone", issue->milestone); json_set_num(j, "comments", issue->comments);
    Json *assignees = json_array(); for (size_t i = 0; i < issue->assignee_count; i++) json_array_push(assignees, json_string(issue->assignees[i])); json_object_set(j, "assignees", assignees);
    Json *labels = json_array(); for (size_t i = 0; i < issue->label_count; i++) { Json *l = json_object(); json_set_str(l, "name", issue->labels[i].name); json_set_str(l, "color", issue->labels[i].color); json_array_push(labels, l); } json_object_set(j, "labels", labels);
    if (issue->has_updated) { char iso[40]; struct tm *tm = gmtime(&issue->updated_at); if (tm) { strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", tm); json_set_str(j, "updatedAt", iso); } }
    Json *sub = json_object(); json_set_num(sub, "total", issue->sub_issues); json_set_num(sub, "completed", issue->sub_issues_done); json_object_set(j, "subIssues", sub);
    Json *link_json(const BoardLink *l);
    if (issue->has_parent) json_object_set(j, "parent", link_json(&issue->parent));
    Json *pulls = json_array(); for (size_t i = 0; i < issue->pull_count; i++) json_array_push(pulls, link_json(&issue->pulls[i])); json_object_set(j, "pulls", pulls);
    issue_summary_parse(j, &s->issue);
    json_free(j);
    return &s->base;
}
Json *link_json(const BoardLink *l) {
    Json *o = json_object();
    json_set_num(o, "number", l->number); json_set_str(o, "title", l->title); json_set_str(o, "url", l->url); json_set_str(o, "repo", l->repo);
    json_set_bool(o, "draft", l->draft); json_set_str(o, "state", l->state); json_set_str(o, "stateReason", l->state_reason);
    Json *labels = json_array(); for (size_t i = 0; i < l->label_count; i++) { Json *x = json_object(); json_set_str(x, "name", l->labels[i].name); json_set_str(x, "color", l->labels[i].color); json_array_push(labels, x); } json_object_set(o, "labels", labels);
    return o;
}
