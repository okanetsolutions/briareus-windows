// The project board: open pull requests and issues, one pull request in full, and one issue.
#include "credentials.h"
#include "dialogs.h"
#include "meeting.h"
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
        { "custom-feedback", "\xE2\x9C\x8D" }, { "test-sheet", "\xF0\x9F\x93\x8B" }, { "test-run", "\xF0\x9F\x8E\xAC" }, { "pr-body-summary", "\xE2\x9C\x8E" }, { "delete-self-comments", "\xF0\x9F\xA7\xB9" },
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

enum { ACT_FILTER = 1000, ACT_TAB, ACT_CLEAR, ACT_OPEN_PULL, ACT_OPEN_ISSUE, ACT_PULL_ACTION, ACT_REFRESH, ACT_FILTER_AUTHOR, ACT_FILTER_REVIEWER, ACT_FILTER_ASSIGNEE, ACT_FILTER_LABEL, ACT_RUNS, ACT_MERGE_PULL, ACT_MEET };
enum { ACT_SSH_BASE = 1100 };   // the SSH sessions tab's own actions, PROJECT_SSH_ACTIONS of them
enum { ACT_SFTP_BASE = 1120 };  // the SFTP sessions tab's, PROJECT_SFTP_ACTIONS of them
enum { ACT_RUN_BASE = 1140 };   // the Run tab's, PROJECT_RUN_ACTIONS of them
enum { ACT_DB_BASE = 1160 };    // the Database tab's, PROJECT_DB_ACTIONS of them
enum { ACT_FORGE_BASE = 1180 }; // the Forge tab's, PROJECT_FORGE_ACTIONS of them
enum { ACT_BOARD_BASE = 1200 }; // the Board tab's, BOARD_TAB_ACTIONS of them
enum { TAB_PULLS, TAB_ISSUES, TAB_SSH, TAB_SFTP, TAB_RUN, TAB_DB, TAB_FORGE, TAB_MEETING, TAB_BOARD };
enum { TIMER_POLL = 1, TIMER_BOARD_RUN_LOG = 3 };
enum { ACTION_STRIDE = 64 };   // ACT_PULL_ACTION's argument: row * stride + errand

typedef struct {
    Screen base;
    Project project;
    Json *board;
    PullSummary *pulls; size_t pull_count;
    IssueSummary *issues; size_t issue_count;
    int tab;   // TAB_PULLS, TAB_ISSUES, TAB_BOARD, TAB_RUN, TAB_SSH, TAB_SFTP, TAB_DB, TAB_FORGE or TAB_MEETING
    ProjectSsh *ssh;   // the SSH sessions tab
    ProjectSftp *sftp; // the SFTP sessions tab
    ProjectRun *run;   // the Run tab, on the default branch
    ProjectDb *db;     // the Database tab
    ProjectForge *forge; // the Forge tab: the project's Forge servers and their sites
    BoardTab *board_tab; // the Board tab: the project's GitHub Projects board
    bool shown;
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
    int merging_number; Request *req_merge;   // the pull request being merged: read for its head first, then merged
    // The Status each linked issue has on its project board, by issue number, from that issue's own read (saved, and
    // shared with the issue and pull request screens). `status_read` holds the ones read this visit; one at a time.
    Json *issue_status, *status_read; Request *req_status;
} PullsScreen;

/// Where the issue screen keeps its read of issue `number`, which the board and the pull request screen share.
static char *saved_issue_key(const char *repo, int number) { return xstrfmt("issue:%s#%d", repo, number); }
/// An issue's Status on the first of its project boards that gives it one; NULL on none.
static const char *issue_project_status(const Json *issue) {
    const Json *projects = json_get(issue, "projects");
    for (size_t i = 0; i < json_count(projects); i++) { const char *st = json_str(json_get(json_at(projects, i), "status")); if (!str_empty(st)) return st; }
    return NULL;
}

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

static char *number_key(int number) { return xstrfmt("%d", number); }
static void status_note(PullsScreen *s, int number, const Json *issue) {
    const char *st = issue_project_status(issue);
    char *k = number_key(number); json_set_str(s->issue_status, k, st ? st : ""); free(k);
}
/// What was saved of the linked issues not known yet, so the statuses show before they are read again.
static void status_restore(PullsScreen *s) {
    for (size_t i = 0; i < s->pull_count; i++) {
        for (size_t k = 0; k < s->pulls[i].issue_count; k++) {
            const BoardLink *l = &s->pulls[i].issues[k];
            char *nk = number_key(l->number);
            bool known = board_link_is_foreign(l, s->project.repo) || json_str(json_get(s->issue_status, nk));
            free(nk);
            if (known) continue;
            char *key = saved_issue_key(s->project.repo, l->number); Json *saved = cache_value(g_store.cache, key); free(key);
            if (saved) { status_note(s, l->number, json_get(saved, "issue")); json_free(saved); }
        }
    }
}
static void status_next(PullsScreen *s);
static void status_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    int number = json_int_or(json_get(req->args, "issue"), 0);
    // Throttling stops the round and leaves this issue unread, so it and the remaining reads wait for the
    // next poll rather than hitting the limit again.
    if (req->error.kind == API_HTTP && req->error.status == 429) return;
    char *k = number_key(number); json_set_bool(s->status_read, k, true); free(k);
    if (req->ok) {
        status_note(s, number, json_get(req->result, "issue"));
        char *key = saved_issue_key(s->project.repo, number); cache_store(g_store.cache, req->result, key); free(key);
        pane_relayout(s->base.pane);
    }
    status_next(s);
}
/// Reads the next linked issue not read this visit: those of the rows the filters show first, then the rest.
static void status_next(PullsScreen *s) {
    if (s->req_status || !s->shown || !store_supports("issue")) return;
    BoardRow *rows = xcalloc(s->pull_count ? s->pull_count : 1, sizeof *rows);
    for (size_t i = 0; i < s->pull_count; i++) rows[i] = pull_board_row(&s->pulls[i]);
    int next = 0;
    for (int pass = 0; pass < 2 && !next; pass++) {
        for (size_t i = 0; i < s->pull_count && !next; i++) {
            if (!pass && !board_filter_passes(&s->pull_filter, &rows[i], -1)) continue;
            for (size_t k = 0; k < s->pulls[i].issue_count && !next; k++) {
                const BoardLink *l = &s->pulls[i].issues[k];
                char *nk = number_key(l->number);
                if (!board_link_is_foreign(l, s->project.repo) && json_is_null(json_get(s->status_read, nk))) next = l->number;
                free(nk);
            }
        }
    }
    free(rows);
    if (!next) return;
    Json *a = json_object(); json_set_num(a, "issue", next); json_set_str(a, "repo", s->project.repo);
    store_call("issue", a, 0, s, status_done, 0, &s->req_status);
}

static void pulls_show(PullsScreen *s, const Json *result, bool saved) {
    json_free(s->board); s->board = json_clone(result);
    pull_summaries_free(s->pulls, s->pull_count); s->pulls = pull_summaries_parse(json_get(result, "pulls"), &s->pull_count);
    issue_summaries_free(s->issues, s->issue_count); s->issues = issue_summaries_parse(json_get(result, "issues"), &s->issue_count);
    board_tab_set_pulls(s->board_tab, s->issues, s->issue_count, s->pulls, s->pull_count);
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
    if (s->tab == TAB_ISSUES && !s->issue_count && !json_is_set(json_get(result, "issuesError"))) s->tab = TAB_PULLS;
    status_restore(s);
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
        status_next(s);
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
static void board_start_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        set_string(&s->write_error, NULL);
        // The list stays shown; the new session joins the runs counted on its pull request.
        if (store_supports("sessions")) { request_cancel(&s->req_runs); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, runs_done, 0, &s->req_runs); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}
static void board_open_run(PullsScreen *s, size_t index);
static void board_start(PullsScreen *s, int number, const char *branch, const BoardAction *action, const char *input) {
    if (s->busy || s->uncertain || str_empty(branch)) return;
    s->busy = true; s->starting_number = number; set_string(&s->starting_id, action->id);
    char *op = board_action_operation(action);
    Json *args = board_action_arguments(action, s->project.repo, number, branch, input);
    store_call(op, args, board_action_timeout_ms(action), s, board_start_done, 0, &s->req_start);
    free(op);
    pane_relayout(s->base.pane);
}

// Merging from the board: the row has no head commit, and the server merges only the head that was read, so the
// pull request is read first and merged at the head that read returns.
static void board_merge_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    s->merging_number = 0;
    if (!req->ok) {
        char *t = request_error_text(req);
        if (!request_outcome_unknown(req)) set_string(&s->write_error, t);
        else { char *m = xstrfmt("%s The merge may still have completed; refresh before trying again.", t); set_string(&s->write_error, m); free(m); }
        free(t);
    } else {
        set_string(&s->write_error, NULL);
        const char *status = json_str(json_get(req->result, "status"));
        if (str_eq(status, "pending")) set_string(&s->write_error, "GitHub accepted the merge and is still finishing it; the pull request leaves the list once it lands.");
    }
    pulls_load(s, true);
    pane_relayout(s->base.pane);
}
static void board_merge_read_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    const Json *pr = json_get(req->result, "pr");
    const char *head = json_str(json_get(pr, "headSha")), *base_ref = json_str(json_get(pr, "baseRef"));
    if (!req->ok || !head || !base_ref || !str_eq(json_str(json_get(pr, "state")), "open")) {
        if (!req->ok) request_error_into(&s->write_error, req);
        else set_string(&s->write_error, str_eq(json_str(json_get(pr, "state")), "open") ? "The server did not say which commit to merge." : "This pull request is no longer open.");
        s->merging_number = 0;
        pane_relayout(s->base.pane);
        return;
    }
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->merging_number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref); json_set_str(args, "method", "squash");
    store_call("merge_pull", args, 0, s, board_merge_done, 0, &s->req_merge);
}
static void board_merge(PullsScreen *s, size_t index) {
    if (index >= s->pull_count || s->merging_number || s->busy) return;
    const PullSummary *pull = &s->pulls[index];
    int number = pull->number;
    Str msg; str_init(&msg);
    str_appendf(&msg, "\xE2\x80\x9C%s\xE2\x80\x9D is squash-merged into %s on GitHub.", pull->title ? pull->title : "", str_empty(pull->base_branch) ? "its base branch" : pull->base_branch);
    if (pull_conflicting(pull)) str_appendz(&msg, "\n\nThis branch has conflicts that must be resolved before it can merge.");
    if (pull_checks_failed(pull)) str_appendz(&msg, "\n\nSome checks failed.");
    else if (str_eq(pull->checks, "pending") || str_eq(pull->checks, "expected")) str_appendz(&msg, "\n\nSome checks are still running.");
    char *title = xstrfmt("Merge #%d?", number);
    s->dialog_open = true;
    bool ok = app_confirm(title, msg.data, "Merge", true);
    s->dialog_open = false;
    free(title); str_free(&msg);
    if (!ok || s->merging_number) return;
    s->merging_number = number; set_string(&s->write_error, NULL);
    Json *args = json_object(); json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", number);
    store_call("pull", args, 0, s, board_merge_read_done, 0, &s->req_merge);
    pane_relayout(s->base.pane);
}

static void pulls_destroy(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_start); request_cancel(&s->req_merge); poller_stop(&s->poller);
    json_free(s->board); json_free(s->catalog); pull_summaries_free(s->pulls, s->pull_count); issue_summaries_free(s->issues, s->issue_count);
    request_cancel(&s->req_runs); sessions_free(s->runs, s->run_count);
    request_cancel(&s->req_status); json_free(s->issue_status); json_free(s->status_read);
    project_ssh_free(s->ssh);
    project_sftp_free(s->sftp);
    project_run_free(s->run);
    project_db_free(s->db);
    project_forge_free(s->forge);
    board_tab_free(s->board_tab);
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

static void pulls_layout(Screen *base, Doc *doc) {
    PullsScreen *s = (PullsScreen *)base;
    int w = doc->width;
    // SSH sessions only for a token that may read the servers; its count is the sessions open on them.
    size_t open = project_ssh_session_count(s->project.repo), files = project_sftp_session_count(s->project.repo);
    char *ssh_label = open ? xstrfmt("\xE2\x9D\xAF SSH sessions %zu", open) : xstrdup("\xE2\x9D\xAF SSH sessions");
    char *sftp_label = files ? xstrfmt("\xE2\x87\xB5 SFTP sessions %zu", files) : xstrdup("\xE2\x87\xB5 SFTP sessions");
    // Run, on the default branch, for a token that may serve one.
    const char *tabs[9] = { "\xE2\x87\x85 Pull requests", "\xE2\x8A\x99 Issues" };
    int ids[9] = { TAB_PULLS, TAB_ISSUES };
    size_t n = 2;
    // Board, after Issues, for a project that names a GitHub Projects board.
    if (board_tab_offered(&s->project)) { tabs[n] = "\xE2\x96\xA6 Board"; ids[n++] = TAB_BOARD; }
    else if (s->tab == TAB_BOARD) s->tab = TAB_PULLS;
    if (project_run_offered()) { tabs[n] = "\xE2\x96\xB6 Run"; ids[n++] = TAB_RUN; }
    if (project_ssh_offered()) { tabs[n] = ssh_label; ids[n++] = TAB_SSH; tabs[n] = sftp_label; ids[n++] = TAB_SFTP; }
    if (project_db_offered()) { tabs[n] = "\xE2\x9B\x81 Database"; ids[n++] = TAB_DB; }
    if (project_forge_offered()) { tabs[n] = "\xE2\x98\x81 Forge"; ids[n++] = TAB_FORGE; }
    // The meeting's transcript, while one runs on this project and after it.
    char *transcript = meeting_transcript_for(s->project.repo);
    if (transcript) { tabs[n] = meeting_for(s->project.repo) ? "\xF0\x9F\x8E\x99 Meeting \xE2\x97\x8F" : "\xF0\x9F\x8E\x99 Meeting"; ids[n++] = TAB_MEETING; }
    else if (s->tab == TAB_MEETING) s->tab = TAB_PULLS;
    doc_tabs(doc, w, tabs, ids, n, s->tab);
    free(ssh_label); free(sftp_label);
    doc_space(doc, px(14));
    if (s->tab == TAB_BOARD) { board_tab_layout(s->board_tab, doc, w); free(transcript); return; }
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
            ButtonSpec *buttons = xcalloc(an + 2, sizeof *buttons); size_t bn = 0;
            char **labels = xcalloc(an + 1, sizeof *labels);
            for (size_t k = 0; k < an && k < ACTION_STRIDE; k++) {
                bool starting = s->busy && s->starting_number == pull->number && str_eq(s->starting_id, actions[k].id);
                bool suggested = str_eq(pull->recommended, actions[k].id);
                labels[k] = starting ? xstrdup("Starting\xE2\x80\xA6") : xstrfmt("%s %s", action_icon(actions[k].id), actions[k].label);
                ButtonSpec b = { 0, labels[k], suggested ? BUTTON_PROMINENT : BUTTON_BORDERED, ACT_PULL_ACTION, (intptr_t)(i * ACTION_STRIDE + k), !s->busy && !s->uncertain };
                buttons[bn++] = b;
            }
            if (!pull->draft && store_can_manage() && store_supports("pull") && store_supports("merge_pull")) {
                bool merging = s->merging_number == pull->number;
                ButtonSpec b = { 0, merging ? "Merging\xE2\x80\xA6" : "\xE2\x86\xB3 Merge", BUTTON_BORDERED, ACT_MERGE_PULL, (intptr_t)i, !s->merging_number && !s->busy };
                buttons[bn++] = b;
            }
            size_t runs = runs_on(s, pull->number);
            char *runs_text = runs ? xstrfmt("%zu run%s \xE2\x80\xBA", runs, runs == 1 ? "" : "s") : NULL;
            if (runs_text) { ButtonSpec b = { 0, runs_text, BUTTON_PLAIN, ACT_RUNS, (intptr_t)i, true }; buttons[bn++] = b; }
            const char **statuses = xcalloc(pull->issue_count ? pull->issue_count : 1, sizeof *statuses);
            for (size_t k = 0; k < pull->issue_count; k++) {
                if (board_link_is_foreign(&pull->issues[k], s->project.repo)) continue;
                char *nk = number_key(pull->issues[k].number); statuses[k] = json_str(json_get(s->issue_status, nk)); free(nk);
            }
            doc_pull_row(doc, 0, w, pull, has_stack ? &stack : NULL, s->project.repo, statuses, ACT_OPEN_PULL, (intptr_t)i, buttons, bn, run_active_on(s, pull->number));
            free(statuses); free(runs_text); str_array_free(labels, an); free(buttons); board_actions_free(actions, an);
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
    // A meeting about this project leads the line: its time, cost and what it is doing.
    if (meeting_for(s->project.repo)) { char *m = meeting_status(); snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s", m, sub.data); free(m); }
    else snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
    str_free(&sub);
    // 🎙 Meet: the meeting assistant, on this project.
    {
        bool here = meeting_for(s->project.repo);
        HeaderButton *m = &info->buttons[info->button_count++];
        snprintf(m->label, sizeof m->label, "%s", here ? "\xF0\x9F\x8E\x99 Meeting \xE2\x97\x8F" : "\xF0\x9F\x8E\x99 Meet");
        m->action = ACT_MEET; m->enabled = true; m->tip = "Join a meeting with an assistant that can look up this project";
    }
    if (s->tab == TAB_MEETING) return;
    if (s->tab == TAB_RUN) { project_run_header(s->run, info); return; }
    if (s->tab == TAB_BOARD) {
        board_tab_header(s->board_tab, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the board from GitHub again";
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
    if (s->tab == TAB_SFTP) {
        project_sftp_header(s->sftp, info);
        HeaderButton *r = &info->buttons[info->button_count++]; r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = true; r->tip = "Read the servers and the folder on show again";
        return;
    }
    // The pickers, as the dashboard's selects, and ⟳.
    {
        BoardFilter *f = current_filter(s);
        const char *author = board_filter_get(f, FILTER_AUTHOR), *reviewer = board_filter_get(f, FILTER_REVIEWER), *assignee = board_filter_get(f, FILTER_ASSIGNEE), *label = board_filter_get(f, FILTER_LABEL);
        HeaderButton *b;
        b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(author) ? "All authors" : author); b->action = ACT_FILTER_AUTHOR; b->enabled = s->loaded;
        if (s->tab == 0) { b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(reviewer) ? "All reviewers" : reviewer); b->action = ACT_FILTER_REVIEWER; b->enabled = s->loaded; }
        if (s->tab == TAB_ISSUES) { b = &info->buttons[info->button_count++]; snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", str_empty(assignee) ? "All assignees" : assignee); b->action = ACT_FILTER_ASSIGNEE; b->enabled = s->loaded; }
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
    if (action == ACT_MEET) { meeting_menu(&s->project, pane_hwnd(base->pane), pt); pane_header_changed(base->pane); return; }
    if (project_ssh_action(s->ssh, action, arg, pt)) return;
    if (project_sftp_action(s->sftp, action, arg, pt)) return;
    if (project_run_action(s->run, action, arg, pt)) return;
    if (project_db_action(s->db, action, arg, pt)) return;
    if (project_forge_action(s->forge, action, arg, pt)) return;
    if (board_tab_action(s->board_tab, action, arg, pt)) return;
    switch (action) {
    case ACT_FILTER_AUTHOR: filter_pick(s, FILTER_AUTHOR, pt); break;
    case ACT_FILTER_REVIEWER: filter_pick(s, FILTER_REVIEWER, pt); break;
    case ACT_FILTER_ASSIGNEE: filter_pick(s, FILTER_ASSIGNEE, pt); break;
    case ACT_FILTER_LABEL: filter_pick(s, FILTER_LABEL, pt); break;
    case ACT_REFRESH: pulls_refresh(base); break;
    case ACT_MERGE_PULL: board_merge(s, (size_t)arg); break;
    case ACT_RUNS: if ((size_t)arg < s->pull_count) app_push_detail(pull_detail_screen_new(&s->project, s->pulls[arg].number, NULL, &s->pulls[arg])); break;
    case ACT_TAB:
        // The side panel belongs to the Board tab's cards.
        if (s->tab == TAB_BOARD && arg != TAB_BOARD) app_set_overlay(NULL);
        s->tab = arg == TAB_ISSUES || ((arg == TAB_SSH || arg == TAB_SFTP) && project_ssh_offered()) || (arg == TAB_RUN && project_run_offered()) || (arg == TAB_DB && project_db_offered()) || (arg == TAB_FORGE && project_forge_offered()) || (arg == TAB_BOARD && board_tab_offered(&s->project)) || arg == TAB_MEETING ? (int)arg : TAB_PULLS;
        if (s->tab == TAB_RUN) project_run_open(s->run);
        if (s->tab == TAB_BOARD) board_tab_open(s->board_tab);
        // The transcript, like a conversation's, keeps to its latest line.
        pane_stick_to_bottom(base->pane, s->tab == TAB_MEETING);
        if (s->tab == TAB_MEETING) pane_scroll_to_bottom(base->pane);
        pane_relayout(base->pane); pane_header_changed(base->pane);
        break;
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
        if (k < an && str_eq(actions[k].id, "run")) board_open_run(s, index);
        else if (k < an) {
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
    if (project_run_timer(s->run, id)) return;
    if (poller_fired(&s->poller, id)) { if (s->dialog_open) poller_finished(&s->poller, false, -1); else pulls_load(s, false); }
}
static void pulls_place(Screen *base, const RECT *content, int scroll_y) {
    PullsScreen *s = (PullsScreen *)base;
    project_ssh_place(s->ssh, content, scroll_y, s->shown && s->tab == TAB_SSH);
    project_sftp_place(s->sftp, content, scroll_y, s->shown && s->tab == TAB_SFTP);
    project_run_place(s->run, content, scroll_y, s->shown && s->tab == TAB_RUN);
    project_forge_place(s->forge, content, scroll_y, s->shown && s->tab == TAB_FORGE);
}
static void pulls_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    project_forge_command(((PullsScreen *)base)->forge, id, code);
}
static bool pulls_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    PullsScreen *s = (PullsScreen *)base;
    return s->tab == TAB_FORGE && project_forge_key(s->forge, vk, ctrl);
}
/// The board gives way to another screen unless a Forge site's unsaved changes are kept.
static bool pulls_can_leave(Screen *base) { return project_forge_can_leave(((PullsScreen *)base)->forge); }
static void pulls_context(Screen *base, int action, intptr_t arg, POINT pt) {
    PullsScreen *s = (PullsScreen *)base;
    project_sftp_context(s->sftp, action, arg, pt);
}
static void pulls_visible(Screen *base, bool shown) {
    PullsScreen *s = (PullsScreen *)base;
    s->shown = shown;
    if (!shown) { project_ssh_place(s->ssh, NULL, 0, false); project_sftp_place(s->sftp, NULL, 0, false); project_run_place(s->run, NULL, 0, false); project_forge_place(s->forge, NULL, 0, false); }
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 45000);
    else { poller_stop(&s->poller); request_cancel(&s->req); request_cancel(&s->req_actions); request_cancel(&s->req_runs); request_cancel(&s->req_status); }
}
static void pulls_refresh(Screen *base) {
    PullsScreen *s = (PullsScreen *)base;
    if (s->tab == TAB_SSH) { project_ssh_refresh(s->ssh); pane_relayout(base->pane); return; }
    if (s->tab == TAB_SFTP) { project_sftp_refresh(s->sftp); pane_relayout(base->pane); return; }
    if (s->tab == TAB_DB) { project_db_refresh(s->db); pane_relayout(base->pane); return; }
    if (s->tab == TAB_FORGE) { project_forge_refresh(s->forge); pane_relayout(base->pane); return; }
    if (s->tab == TAB_RUN) { project_run_refresh(s->run); return; }
    if (s->tab == TAB_BOARD) { board_tab_refresh(s->board_tab); return; }
    // Refreshing is how an uncertain start is checked: its conversation is listed in the project if it began.
    s->uncertain = false; set_string(&s->write_error, NULL);
    request_cancel(&s->req_status); json_free(s->status_read); s->status_read = json_object();
    pulls_load(s, true);
}
static void pulls_activated(Screen *base, bool active) { if (active) pulls_visible(base, true); }
static const ScreenVTable pulls_vt = {
    .destroy = pulls_destroy, .layout = pulls_layout, .header = pulls_header, .action = pulls_action, .timer = pulls_timer,
    .visible = pulls_visible, .refresh = pulls_refresh, .activated = pulls_activated, .place = pulls_place, .context = pulls_context,
    .command = pulls_command, .key = pulls_key, .can_leave = pulls_can_leave,
};
Screen *pulls_screen_new(const Project *project) {
    PullsScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &pulls_vt; s->base.id = xstrfmt("pulls:%s", project->repo);
    project_copy(&s->project, project);
    s->board = json_null(); s->catalog = json_array(); s->issue_status = json_object(); s->status_read = json_object();
    board_filter_init(&s->pull_filter); board_filter_init(&s->issue_filter); board_filter_init(&s->opening);
    s->has_opening = !filters_restore(s);
    s->ssh = project_ssh_new(project->repo, &s->base, ACT_SSH_BASE);
    s->sftp = project_sftp_new(project->repo, &s->base, ACT_SFTP_BASE);
    s->run = project_run_new(project->repo, &s->base, ACT_RUN_BASE, TIMER_BOARD_RUN_LOG);
    s->db = project_db_new(project->repo, &s->base, ACT_DB_BASE);
    s->forge = project_forge_new(project->repo, &s->base, ACT_FORGE_BASE);
    s->board_tab = board_tab_new(project, &s->base, ACT_BOARD_BASE);
    return &s->base;
}

// MARK: - Edits, shared by one pull request and one issue

// The user's own GitHub login, which "Assign me" adds: asked the first time and kept in the registry, since the API does
// not say whose the token is.
static char *my_login;
static bool my_login_read;
static const char *github_login(void) {
    if (!my_login_read) { my_login = settings_read_github_login(); my_login_read = true; }
    return my_login;
}
/// Asks for the login, the saved one filled in; false when cancelled or left blank.
static bool github_login_ask(void) {
    char *typed = dialog_text(app_window(), "Your GitHub login", "The GitHub user \xE2\x80\x9C" "Assign me\xE2\x80\x9D adds", "Save", github_login() ? github_login() : "");
    if (!typed) return false;
    Json *names = board_names_parse(typed, true);
    const char *login = json_str(json_at(names, 0));
    if (login) { set_string(&my_login, login); settings_write_github_login(login); }
    free(typed); json_free(names);
    return login != NULL;
}
/// "Assign me" or "Unassign me" as the item's assignees stand.
static const char *assign_me_label(char *const *assignees, size_t count) {
    const char *me = github_login();
    for (size_t i = 0; me && i < count; i++) if (str_ieq(assignees[i], me)) return "Unassign me";
    return "Assign me";
}
enum { EDIT_ASSIGN_ME = 1, EDIT_ASSIGNEES, EDIT_LOGIN };
/// The ▾ beside the assignees: assign or unassign the user, edit the list, or change which login "me" is.
static int assignees_menu(HWND hwnd, POINT pt, char *const *assignees, size_t count) {
    HMENU menu = CreatePopupMenu();
    wchar_t *me = utf8_to_wide(assign_me_label(assignees, count));
    AppendMenuW(menu, MF_STRING, EDIT_ASSIGN_ME, me); free(me);
    AppendMenuW(menu, MF_STRING, EDIT_ASSIGNEES, L"Edit assignees\x2026");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    char *change = github_login() ? xstrfmt("Change my GitHub login (%s)\xE2\x80\xA6", github_login()) : xstrdup("Set my GitHub login\xE2\x80\xA6");
    wchar_t *w = utf8_to_wide(change); AppendMenuW(menu, MF_STRING, EDIT_LOGIN, w); free(w); free(change);
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    return chosen;
}
/// What a pick from that menu sends as `assignees`, or NULL for nothing to send. The list replaces GitHub's whole.
static Json *assignees_chosen(int chosen, char *const *assignees, size_t count, int number) {
    if (chosen == EDIT_LOGIN) { github_login_ask(); return NULL; }
    if (chosen == EDIT_ASSIGN_ME) {
        if (!github_login() && !github_login_ask()) return NULL;
        return board_assignees_toggle(assignees, count, github_login(), NULL);
    }
    if (chosen != EDIT_ASSIGNEES) return NULL;
    char *current = board_names_join(assignees, count), *caption = xstrfmt("Assignees of #%d", number);
    char *typed = dialog_text(app_window(), caption, "GitHub logins, comma separated (ten at most)", "Save", current);
    free(current); free(caption);
    if (!typed) return NULL;
    Json *names = board_names_parse(typed, true);
    free(typed);
    return names;
}
/// The menu, then what its pick sends. The assignees are copied first: the screen may read them again while it is up.
static Json *assignees_edit(HWND hwnd, POINT pt, char *const *assignees, size_t count, int number) {
    char **mine = xcalloc(count + 1, sizeof *mine);
    for (size_t i = 0; i < count; i++) mine[i] = xstrdup(assignees[i]);
    Json *out = assignees_chosen(assignees_menu(hwnd, pt, mine, count), mine, count, number);
    str_array_free(mine, count);
    return out;
}
/// The labels typed over the current ones (read before the dialog opens), or NULL when cancelled.
static Json *labels_edit(const PullLabel *labels, size_t count, int number) {
    char *current = board_label_names_join(labels, count), *caption = xstrfmt("Labels of #%d", number);
    char *typed = dialog_text(app_window(), caption, "Label names, comma separated", "Save", current);
    free(current); free(caption);
    if (!typed) return NULL;
    Json *names = board_names_parse(typed, false);
    free(typed);
    return names;
}
/// The title and description after the edit dialog, as only the fields that changed; NULL when cancelled or unchanged.
static Json *details_edit(const char *what, int number, const char *title, const char *body) {
    char *caption = xstrfmt("Edit %s #%d", what, number);
    char *t = xstrdup(title ? title : ""), *b = xstrdup(body ? body : "");
    bool ok = dialog_edit_item(app_window(), caption, &t, &b);
    free(caption);
    Json *fields = NULL;
    if (ok) {
        char *was = str_replace(body ? body : "", "\r\n", "\n");
        fields = json_object();
        if (!str_eq(t, title)) json_set_str(fields, "title", t);
        if (!str_eq(b, was)) json_set_str(fields, "body", b);
        free(was);
        if (!json_count(fields)) { json_free(fields); fields = NULL; }
    }
    free(t); free(b);
    return fields;
}

// MARK: - Pull request

enum {
    ACT_OPEN_URL = 1100, ACT_STACK_ITEM, ACT_STACK_TOGGLE, ACT_PR_TAB, ACT_FINDING_TOGGLE, ACT_FINDING_DECISION, ACT_MERGE,
    ACT_START_ACTION, ACT_OPEN_RUN, ACT_CHECK_URL, ACT_COMMIT_URL, ACT_ISSUE_URL, ACT_FINDING_URL, ACT_RELOAD, ACT_SOLVE_FINDINGS, ACT_CONV_URL,
    ACT_DELETE_RUN, ACT_DELETE_SERVED, ACT_WEB_RELOAD, ACT_WEB_BROWSER, ACT_RUN_PROFILE, ACT_RUN_RETRY, ACT_PR_PROJECT,
    ACT_EDIT, ACT_EDIT_ASSIGNEES, ACT_EDIT_LABELS, ACT_UPDATE_BRANCH,
    ACT_FILES_BASE = 1200,   // the Files changed tab's own actions, PULL_FILES_ACTIONS of them
};
enum { TIMER_FILES_PAGE = 2, TIMER_RUN_LOG };
enum { TAG_PULL = 1, TAG_FINDINGS, TAG_ROWS, TAG_ACTIONS, TAG_SESSIONS, TAG_START, TAG_MERGE, TAG_DECIDE, TAG_BODY, TAG_CONV, TAG_DELETE_RUN, TAG_RUN, TAG_PROFILES, TAG_RUN_LOG, TAG_ACCESS, TAG_ISSUE, TAG_EDIT, TAG_UPDATE_BRANCH };
enum { PR_TAB_BODY, PR_TAB_CONVERSATION, PR_TAB_SESSIONS, PR_TAB_FILES, PR_TAB_COMMITS, PR_TAB_CHECKS, PR_TAB_FINDINGS, PR_TAB_RUN };

/// One of the Conversation tab's lists, read page by page: `incoming` fills up and replaces `items` once the last page is in.
enum { CONV_COMMENTS, CONV_REVIEWS, CONV_REVIEW_COMMENTS, CONV_FEEDS };
static const struct { const char *op, *field; } conv_feeds[CONV_FEEDS] = {
    { "pull_comments", "comments" }, { "pull_reviews", "reviews" }, { "pull_review_comments", "reviewComments" },
};
enum { LINKED_ISSUES_MAX = 10 };
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
    bool busy, uncertain, merging;
    // An edit of its title, description, labels or assignees, and an update of its branch from its base: `branch_note`
    // says GitHub took the update, which it makes on its own a moment later.
    bool editing, updating_branch;
    char *edit_error, *branch_note;
    Request *req_edit, *req_update_branch;
    bool runs_read;     // the Sessions list was read once
    // The Run tab: opening it serves the pull request (▶ Run) and shows it in an embedded browser laid over the tab's area
    // (`web_rc`, in document coordinates). `run_session` is the session serving it, `run_profile` the run profile it
    // serves, `run_want` the one picked in the tab, `run_asked` the one the request in flight asked for.
    char *run_url, *run_session, *run_profile, *run_want, *run_asked, *serve_error;
    bool run_busy, run_pending;   // pending: waits for the Sessions list, in case a Run already serves this pull request
    char **profiles; size_t profile_count; bool profiles_read;   // the project's run profiles, the default first
    Request *req_run, *req_profiles;
    // The log of the Run under way, from its session's transcript: `log_session` is the session followed (found in the
    // Sessions list while `serve_pull` prepares it).
    char *log_session; RunLog log; Request *req_log;
    WebView *web; RECT web_rc; bool shown;
    // The Cloudflare Access service token the browser sends to preview hosts, read once before it first opens; without
    // one (an older server, or none configured) the page asks for a sign-in instead.
    char *access_id, *access_secret, *access_suffix; bool access_read; Request *req_access;
    int tab;
    char *body, *body_author;   // the description, from `pull_files` when `pull` leaves it out
    bool body_read;
    char *deciding;
    // The projects of the issues it closes, as GitHub's sidebar shows them: one `issue` read per linked issue in this
    // repository, once per visit and on refresh, kept as [{number, projects, projectsError}] in the sidebar's order.
    // The round keeps the issue numbers it reads, so a list that changes partway starts it again.
    Json *issue_projects, *issue_incoming; bool issues_read; Request *req_issue;
    int issue_round[LINKED_ISSUES_MAX]; size_t issue_round_count;
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
    // ▶ Run is the Run tab here.
    for (size_t i = 0; i < s->action_count; i++) {
        if (!str_eq(s->actions[i].id, "run")) continue;
        board_action_free(&s->actions[i]);
        memmove(&s->actions[i], &s->actions[i + 1], (s->action_count - i - 1) * sizeof *s->actions);
        s->action_count--;
        break;
    }
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

/// The issues the Development item lists that are in this repository: the board's row first, else the pull request's own.
static size_t linked_issues(PullScreen *s, int *out, size_t cap) {
    const PullSummary *row = board_row(s);
    size_t n = 0;
    if (row && row->issue_count) {
        for (size_t i = 0; i < row->issue_count && n < cap; i++) if (!board_link_is_foreign(&row->issues[i], s->project.repo)) out[n++] = row->issues[i].number;
        return n;
    }
    const Json *arr = json_get(s->pr, "issues");
    for (size_t i = 0; i < json_count(arr) && n < cap; i++) {
        BoardLink l;
        if (!board_link_parse(json_at(arr, i), &l)) continue;
        if (!board_link_is_foreign(&l, s->project.repo)) out[n++] = l.number;
        board_link_free(&l);
    }
    return n;
}
static bool issue_round_same(const PullScreen *s, const int *numbers, size_t n) {
    return n == s->issue_round_count && !memcmp(numbers, s->issue_round, n * sizeof *numbers);
}
static Json *linked_issue_entry(int number, const Json *issue) {
    Json *e = json_object();
    json_set_num(e, "number", number);
    json_object_set(e, "projects", json_clone(json_get(issue, "projects")));
    if (json_str(json_get(issue, "projectsError"))) json_set_str(e, "projectsError", json_str(json_get(issue, "projectsError")));
    return e;
}
static void issue_projects_next(PullScreen *s);
static void issue_projects_done(void *owner, Request *req) {
    PullScreen *s = owner;
    // Throttling stops the round with this issue unread, so it and the rest are read on the next poll rather
    // than hitting the limit again.
    if (req->error.kind == API_HTTP && req->error.status == 429) return;
    double number = 0; json_num(json_get(req->args, "issue"), &number);
    if (req->ok) {
        json_array_push(s->issue_incoming, linked_issue_entry((int)number, json_get(req->result, "issue")));
        char *key = saved_issue_key(s->project.repo, (int)number); cache_store(g_store.cache, req->result, key); free(key);
    } else {
        // One that cannot be read keeps what was shown of it, if anything; the others still show.
        const Json *kept = NULL;
        for (size_t i = 0; i < json_count(s->issue_projects) && !kept; i++) if (json_int_or(json_get(json_at(s->issue_projects, i), "number"), 0) == (int)number) kept = json_at(s->issue_projects, i);
        json_array_push(s->issue_incoming, kept ? json_clone(kept) : json_null());
    }
    issue_projects_next(s);
}
/// Reads the next linked issue, or, once they are all in, shows what they said.
static void issue_projects_next(PullScreen *s) {
    int numbers[LINKED_ISSUES_MAX]; size_t n = linked_issues(s, numbers, LINKED_ISSUES_MAX);
    // A list that changed partway, as when the board's row replaces the saved one, starts the round again on it.
    if (!issue_round_same(s, numbers, n)) {
        memcpy(s->issue_round, numbers, n * sizeof *numbers); s->issue_round_count = n;
        json_free(s->issue_incoming); s->issue_incoming = json_array();
    }
    size_t at = json_count(s->issue_incoming);
    if (at < n) {
        Json *a = json_object(); json_set_num(a, "issue", numbers[at]); json_set_str(a, "repo", s->project.repo);
        store_call("issue", a, 0, s, issue_projects_done, TAG_ISSUE, &s->req_issue);
        return;
    }
    Json *done = json_array();
    for (size_t i = 0; i < json_count(s->issue_incoming); i++) if (!json_is_null(json_at(s->issue_incoming, i))) json_array_push(done, json_clone(json_at(s->issue_incoming, i)));
    json_free(s->issue_incoming); s->issue_incoming = NULL;
    json_free(s->issue_projects); s->issue_projects = done;
    s->issues_read = true;
    pane_relayout(s->base.pane);
}
/// The linked issues' projects: what was saved of them at once, then each issue read again.
static void issue_projects_load(PullScreen *s) {
    if (s->req_issue || !store_supports("issue")) return;
    int numbers[LINKED_ISSUES_MAX]; size_t n = linked_issues(s, numbers, LINKED_ISSUES_MAX);
    // Read once per visit, unless the issues it links have changed since.
    if (s->issues_read && issue_round_same(s, numbers, n)) return;
    s->issues_read = false;
    // A pull request that no longer links any issue drops the projects it showed.
    if (!n) { if (s->issue_projects) { json_free(s->issue_projects); s->issue_projects = NULL; pane_relayout(s->base.pane); } return; }
    if (!s->issue_projects) {
        Json *saved_all = json_array();
        for (size_t i = 0; i < n; i++) {
            char *key = saved_issue_key(s->project.repo, numbers[i]); Json *saved = cache_value(g_store.cache, key); free(key);
            if (saved) { json_array_push(saved_all, linked_issue_entry(numbers[i], json_get(saved, "issue"))); json_free(saved); }
        }
        if (json_count(saved_all)) s->issue_projects = saved_all; else json_free(saved_all);
    }
    // A round throttled partway carries on where it stopped.
    if (!s->issue_incoming) s->issue_incoming = json_array();
    issue_projects_next(s);
}

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
    issue_projects_load(s);
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
    issue_projects_load(s);
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
static void run_resume(PullScreen *s);
static void sessions_done_pull(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = xcalloc(n, sizeof *s->runs); s->run_count = 0;
    for (size_t i = 0; i < n; i++) if (session_pull_number(&all[i]) == s->number) session_copy(&s->runs[s->run_count++], &all[i]);
    sessions_free(all, n);
    s->runs_read = true;
    if (s->run_pending) { s->run_pending = false; run_resume(s); }
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
    request_cancel(&s->req_issue); json_free(s->issue_projects); json_free(s->issue_incoming);
    request_cancel(&s->req_edit); request_cancel(&s->req_update_branch); free(s->edit_error); free(s->branch_note);
    poller_stop(&s->poller);
    project_free(&s->project); if (s->has_stack) stack_position_free(&s->stack); if (s->has_summary) pull_summary_free(&s->summary);
    json_free(s->pr); if (s->has_row) pull_summary_free(&s->row); json_free(s->catalog); sessions_free(s->runs, s->run_count); json_free(s->findings);
    free(s->error); free(s->findings_error); free(s->write_error); free(s->merge_error); free(s->deciding); free(s->open_findings); free(s->body); free(s->body_author);
    board_actions_free(s->actions, s->action_count);
    pull_files_free(s->files);
    request_cancel(&s->req_run); request_cancel(&s->req_profiles); request_cancel(&s->req_log);
    run_log_clear(&s->log); free(s->log_session);
    webview_free(s->web);
    request_cancel(&s->req_access); free(s->access_id); free(s->access_secret); free(s->access_suffix);
    free(s->run_url); free(s->run_session); free(s->run_profile); free(s->run_want); free(s->run_asked); free(s->serve_error);
    str_array_free(s->profiles, s->profile_count);
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

/// The description as last read, NULL until it is: an edit starts from it.
static const char *pull_body(PullScreen *s) {
    const char *body = json_str(json_get(s->pr, "body"));
    return body ? body : s->body;
}
/// `gh-header`: the title with its muted number and the buttons beside it, then the state and `author wants to merge N commits into base from head`.
static void layout_header(PullScreen *s, Doc *doc, Col c) {
    const PullSummary *row = board_row(s);
    bool can_merge = store_supports("merge_pull") && str_eq(json_str(json_get(s->pr, "state")), "open") && json_bool_tristate(json_get(s->pr, "draft")) != 1
        && json_str(json_get(s->pr, "headSha")) && json_str(json_get(s->pr, "baseRef"));
    bool can_update_branch = store_supports("update_pull_branch") && str_eq(json_str(json_get(s->pr, "state")), "open")
        && json_str(json_get(s->pr, "headSha")) && json_str(json_get(s->pr, "baseRef"));
    ButtonSpec buttons[5]; size_t bn = 0;
    { ButtonSpec b = { 0, "\xE2\x9F\xB3 Refresh", BUTTON_BORDERED, ACT_RELOAD, 0, !s->req_pull }; buttons[bn++] = b; }
    if (store_supports("update_pull") && is_open(s)) { ButtonSpec b = { 0, s->editing ? "Saving\xE2\x80\xA6" : "Edit", BUTTON_BORDERED, ACT_EDIT, 0, !s->editing && pull_body(s) }; buttons[bn++] = b; }
    if (can_update_branch) { ButtonSpec b = { 0, s->updating_branch ? "Updating\xE2\x80\xA6" : "Update branch", BUTTON_BORDERED, ACT_UPDATE_BRANCH, 0, !s->updating_branch && !s->merging }; buttons[bn++] = b; }
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
    if (s->edit_error) { doc_space(doc, px(8)); doc_notice(doc, c.ix, c.iw, s->edit_error); }
    if (s->branch_note) { doc_space(doc, px(8)); doc_label(doc, c.ix, c.iw, 0xE895, s->branch_note, FONT_FOOTNOTE, theme.secondary); }
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
static const char *run_shown_profile(PullScreen *s);
static void paint_delete_run(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
/// The trash button at the left end of the open Run tab: the button, with the tab's underline running on beneath it.
static void paint_delete_run_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT u = { rc->left + px(4), rc->bottom - px(2), rc->right + px(12), rc->bottom };
    fill_round_rect(cv, &u, px(1), theme.accent, theme.accent);
    int bh = px(26), top = rc->top + (rc->bottom - px(2) - rc->top - bh) / 2;
    RECT b = { rc->left + px(4), top, rc->left + px(4) + px(28), top + bh };
    paint_delete_run(doc, it, cv, &b);
}
/// The session the Run tab shows: the one serving it, else the one being prepared for it.
static const char *run_target(PullScreen *s) { return s->run_session ? s->run_session : s->log_session; }
static void layout_tabs(PullScreen *s, Doc *doc, Col c) {
    int h = px(42), x = c.ix, y = doc->y, right = c.ix + c.iw;
    bool loaded = !json_is_null(s->pr);
    double additions = 0, deletions = 0;
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
    if (store_supports("serve_pull") || s->run_url) {
        // With run profiles, the tab names the one chosen and, once open, picks another.
        bool on = s->tab == PR_TAB_RUN;
        char *title = s->profile_count ? xstrfmt("Run \xC2\xB7 %s \xE2\x96\xBE", run_shown_profile(s)) : xstrdup("Run");
        doc_tab(doc, &x, &y, c.ix, right, h, 0xE768, title, NULL, on, ACT_PR_TAB, PR_TAB_RUN);
        free(title);
        Item *tab = doc_item(doc, (int)doc->count - 1);
        if (on && s->profile_count) { tab->action = ACT_RUN_PROFILE; tab->hand = true; }
        // Inside the open tab, at its left end, a trash button deletes the run's session, and the workspace serving it
        // with it. The tab moves right to make room, and the button carries the tab's underline on under itself.
        if (on && run_target(s) && store_supports("delete")) {
            int slot = px(28);
            RECT br = { tab->rc.left, tab->rc.top, tab->rc.left + slot, tab->rc.bottom };
            OffsetRect(&tab->rc, slot, 0); x += slot;
            Item *it = doc_item(doc, doc_add(doc, &br, paint_delete_run_tab));
            // cppcheck-suppress intToPointerCast
            it->data = str_eq(s->deleting_run, run_target(s)) ? (void *)1 : NULL;   // a marker only, never freed
            if (!s->deleting_run) { it->action = ACT_DELETE_SERVED; it->hand = true; }
        }
    }
    if (ds) { doc->y = y; doc_custom(doc, c.ix + c.iw - dsw, dsw, h, paint_diffstat, ds, free, 0, 0); }
    doc->y = y + h;
    doc_rule(doc, c.x, c.w);
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
/// A timeline comment: a box opened with its header (`h`, which is taken). Returns the box for doc_box_end, with the
/// inner column in `ix`/`iw`. A header with an action opens the comment on GitHub.
static int comment_begin(Doc *doc, Col c, CommentHeadData *h, int action, intptr_t arg, int *ix, int *iw) {
    int x = c.x, w = c.w;
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
            // cppcheck-suppress intToPointerCast
            it->data = mine ? (void *)1 : NULL;   // a marker only, never freed
            if (!s->deleting_run) { it->action = ACT_DELETE_RUN; it->arg = (intptr_t)i; it->hand = true; }
        }
        doc_space(doc, px(6));
    }
}

/// The Run tab's session was deleted, and the workspace serving it with it: the tab forgets it and gives way to the PR
/// body; opening it again prepares a new one.
static void run_forget(PullScreen *s) {
    request_cancel(&s->req_run); request_cancel(&s->req_log);
    s->run_busy = false; s->run_pending = false;
    set_string(&s->run_session, NULL); set_string(&s->run_url, NULL); set_string(&s->run_profile, NULL);
    set_string(&s->run_asked, NULL); set_string(&s->serve_error, NULL);
    set_string(&s->log_session, NULL); run_log_clear(&s->log);
    webview_free(s->web); s->web = NULL;
    if (s->tab == PR_TAB_RUN) s->tab = PR_TAB_BODY;
    if (s->base.pane) { KillTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG); pane_header_changed(s->base.pane); }
}
static void delete_run_done(void *owner, Request *req) {
    PullScreen *s = owner;
    const char *id = json_str(json_get(req->args, "sessionId"));
    if (req->ok) {
        set_string(&s->run_error, NULL);
        if (str_eq(id, s->run_session) || str_eq(id, s->log_session)) run_forget(s);
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
/// The edit button under the sidebar's assignees or labels.
static void side_edit_button(Doc *doc, Col c, const char *text, int action, bool enabled) {
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
static void side_project(Doc *doc, Col c, const Json *p, const char *from, int action, intptr_t arg) {
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
static void side_projects_none(Doc *doc, Col c, const char *refused) {
    if (refused) { char *t = xstrfmt("GitHub would not read its projects with the server\xE2\x80\x99s token, which needs Projects: read. %s", refused); doc_text(doc, c.x, c.w, t, FONT_CAPTION, theme.secondary, DT_WORDBREAK); free(t); }
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
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
    if (store_supports("update_pull") && is_open(s)) side_edit_button(doc, c, "Edit assignees \xE2\x96\xBE", ACT_EDIT_ASSIGNEES, !s->editing);
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
    if (store_supports("update_pull") && is_open(s)) side_edit_button(doc, c, "Edit labels\xE2\x80\xA6", ACT_EDIT_LABELS, !s->editing);
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
    side_heading(doc, c, count, "Projects");
    const char *refused = NULL;
    for (size_t i = 0; i < issues; i++) {
        const Json *e = json_at(s->issue_projects, i), *projects = json_get(e, "projects");
        if (!refused) refused = json_str(json_get(e, "projectsError"));
        char *from = issues > 1 ? xstrfmt("#%d", json_int_or(json_get(e, "number"), 0)) : NULL;
        for (size_t k = 0; k < json_count(projects); k++) {
            if (shown++) doc_space(doc, px(10));
            side_project(doc, c, json_at(projects, k), from, ACT_PR_PROJECT, (intptr_t)(i << 16 | k));
        }
        free(from);
    }
    if (!shown) side_projects_none(doc, c, refused);
}
/// The sidebar: only what the server reports; the projects are those of the issues it closes, and notifications are not
/// part of it.
static void layout_sidebar(PullScreen *s, Doc *doc, Col c) {
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

/// The profile the tab shows as chosen: the one picked, else the one served, else the project's default.
static const char *run_shown_profile(PullScreen *s) {
    if (s->run_want) return s->run_want;
    if (s->run_profile) return s->run_profile;
    return s->profile_count ? s->profiles[0] : NULL;
}
/// The Run tab: the browser's area, down to the bottom of the pane, under a line saying what a restart is doing, with
/// what is happening written in it until the page is up. The tab itself is the run profile dropdown.
static void layout_run(PullScreen *s, Doc *doc, int w) {
    RECT view = pane_content_rect(s->base.pane);
    if (s->run_error) { doc_notice(doc, px(4), w - px(8), s->run_error); doc_space(doc, px(10)); }
    bool page = s->run_url && !s->run_busy && s->web && webview_ready(s->web);
    if (page && s->serve_error) { doc_text(doc, px(4), w - px(8), s->serve_error, FONT_FOOTNOTE, theme.danger, DT_SINGLELINE | DT_END_ELLIPSIS); doc_space(doc, px(10)); }
    int area = doc->y, h = (view.bottom - view.top) - area - px(12);
    if (h < px(320)) h = px(320);
    SetRect(&s->web_rc, 0, area, w, area + h);
    if (page) { doc->y = area + h; return; }
    const char *error = s->run_busy ? NULL : !s->run_url ? s->serve_error : s->web ? webview_error(s->web) : NULL;
    char *text = error ? xstrdup(error)
        : s->run_busy && s->run_url ? (s->run_asked ? xstrfmt("Restarting with profile %s\xE2\x80\xA6", s->run_asked) : xstrdup("Serving it again\xE2\x80\xA6"))
        : s->run_url ? xstrdup("Starting the browser\xE2\x80\xA6")
        : s->run_busy && s->run_session ? xstrdup("Serving it\xE2\x80\xA6")
        // Once the setup's console has lines it says what is happening; until then, a line saying what is coming.
        : s->log.count ? NULL
        : xstrdup("Preparing a workspace for this pull request and serving it with the project\xE2\x80\x99s run commands. This can take a few minutes\xE2\x80\xA6");
    if (text) doc_text(doc, px(4), w - px(8), text, FONT_BODY, error ? theme.danger : theme.muted, DT_WORDBREAK);
    free(text);
    if (!s->run_url && s->serve_error && !s->run_busy) {
        doc_space(doc, px(10));
        doc_button(doc, px(4), 0, "\xE2\x96\xB6 Try again", BUTTON_BORDERED, ACT_RUN_RETRY, 0, true);
    } else if (error && s->run_url) {
        doc_space(doc, px(8));
        int i = doc_text(doc, px(4), w - px(8), "Open in your browser instead \xE2\x86\x97", FONT_BODY, theme.accent, DT_SINGLELINE);
        doc_item(doc, i)->action = ACT_WEB_BROWSER; doc_item(doc, i)->hand = true;
    }
    // The setup as it happens, its latest lines filling what is left of the area, as a terminal does.
    if (s->log.count && (s->run_busy || !s->run_url)) {
        doc_space(doc, px(12));
        int line = px(17), room = area + h - doc->y - px(24);
        size_t fit = room > line ? (size_t)(room / line) : 1, first = s->log.count > fit ? s->log.count - fit : 0;
        int box = doc_box_begin(doc, px(4), w - px(8), px(10), theme.sunken, theme.line, px(6));
        doc_item(doc, box)->hover_fill = false;
        for (size_t i = first; i < s->log.count; i++)
            doc_text(doc, px(16), w - px(32), s->log.lines[i], FONT_MONO_SMALL, s->log.errors[i] ? theme.danger : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        doc_box_end(doc, box, px(10));
    }
    if (doc->y < area + h) doc->y = area + h;
}
static void log_follow(PullScreen *s, const char *session) {
    if (str_eq(s->log_session, session)) return;
    set_string(&s->log_session, session);
    run_log_clear(&s->log);
}
static void log_events_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok || !str_eq(json_str(json_get(req->args, "sessionId")), s->log_session)) return;
    if (run_log_add_events(&s->log, json_get(req->result, "events")) && s->tab == PR_TAB_RUN && s->base.pane) pane_relayout(s->base.pane);
}
/// While `serve_pull` prepares the session, its id is not known yet: the newest ▶ Run session on this pull request is it.
static void log_find_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok || s->run_session) return;
    char *id = run_session_preparing(req->result, s->number);
    if (id) log_follow(s, id);
    free(id);
}
static void run_log_tick(PullScreen *s) {
    if (!s->run_busy) { if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG); return; }
    if (s->req_log) return;
    if (s->run_session) log_follow(s, s->run_session);
    if (s->log_session && store_supports("session")) {
        Json *a = json_object(); json_set_str(a, "sessionId", s->log_session); json_set_num(a, "since", s->log.cursor);
        store_call("session", a, 0, s, log_events_done, TAG_RUN_LOG, &s->req_log);
    } else if (!s->run_session && store_supports("sessions")) {
        Json *a = json_object(); json_set_str(a, "repo", s->project.repo);
        store_call("sessions", a, 0, s, log_find_done, TAG_RUN_LOG, &s->req_log);
    }
}
static void run_changed(PullScreen *s) { if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); } }
static void web_changed(void *ctx) {
    PullScreen *s = ctx;
    if (s->base.pane && pane_top(s->base.pane) == &s->base) run_changed(s);
}
/// Shows the served address in the Run tab, starting the browser again when the address changed.
static void show_run(PullScreen *s, const char *url) {
    if (!str_eq(s->run_url, url)) { webview_free(s->web); s->web = NULL; set_string(&s->run_url, url); }
    run_changed(s);
}
static void profiles_done(void *owner, Request *req) {
    PullScreen *s = owner;
    if (!req->ok) return;
    s->profiles_read = true;
    str_array_free(s->profiles, s->profile_count);
    s->profiles = run_profiles_parse(req->result, s->project.repo, &s->profile_count);
    run_changed(s);
}
static void run_start(PullScreen *s);
static void run_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->run_busy = false;
    bool switched = json_str(json_get(req->args, "sessionId")) != NULL;
    char *asked = s->run_asked; s->run_asked = NULL;
    if (!req->ok) {
        request_error_into(&s->serve_error, req);
        if (switched && (req->error.status == 404 || (req->error.message && strstr(req->error.message, "no live workspace")))) {
            // The session closed or expired, and its page with it: the next try prepares a new one.
            set_string(&s->run_session, NULL); set_string(&s->run_url, NULL);
            webview_free(s->web); s->web = NULL;
        } else if (switched) {
            // A refused switch leaves the app serving what it served.
            set_string(&s->run_want, s->run_profile);
        }
    } else {
        const char *id = json_str(json_get(json_get(req->result, "session"), "id"));
        if (id) set_string(&s->run_session, id);
        set_string(&s->run_profile, json_str(json_get(req->result, "profile")));
        const char *url = json_str(json_get(req->result, "url"));
        if (safe_web_url(url)) {
            bool same = str_eq(url, s->run_url);
            show_run(s, url);
            // The same address after a restart is a new app behind it.
            if (same && s->web) webview_reload(s->web);
        } else set_string(&s->serve_error, "The server did not say where it serves the pull request.");
        // A profile picked while this request was out, or a new session served with the default.
        if (s->run_want && s->run_session && !str_eq(s->run_want, asked) && !str_eq(s->run_want, s->run_profile)) { free(asked); run_start(s); return; }
        if (store_supports("sessions")) { request_cancel(&s->req_sessions); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    }
    free(asked);
    run_changed(s);
}
/// Serves the pull request: the run's session again, with the picked profile, when there is one; else a new session,
/// which the server serves with the project's default profile (switched afterwards when another was picked).
static void run_start(PullScreen *s) {
    if (s->run_busy) return;
    s->run_busy = true; set_string(&s->serve_error, NULL);
    Json *args = json_object();
    const char *op = "serve_pull";
    if (s->run_session && store_supports("serve")) {
        op = "serve";
        json_set_str(args, "sessionId", s->run_session);
        if (s->run_want) json_set_str(args, "profile", s->run_want);
        set_string(&s->run_asked, s->run_want);
    } else { json_set_str(args, "repo", s->project.repo); json_set_num(args, "prNumber", s->number); }
    store_call(op, args, 170000, s, run_done, TAG_RUN, &s->req_run);
    if (s->run_asked) { char *t = xstrfmt("\xE2\x96\xB6 Switching to profile %s", s->run_asked); run_log_add(&s->log, t, false); free(t); }
    if (s->base.pane) SetTimer(pane_hwnd(s->base.pane), TIMER_RUN_LOG, 1500, NULL);
    run_changed(s);
}
/// A Run already serving this pull request (one of its sessions with serve links) is shown as it is.
static bool run_adopt(PullScreen *s) {
    Json *rows = json_array();
    for (size_t i = 0; i < s->run_count; i++) json_array_push(rows, json_clone(s->runs[i].raw));
    char *id = NULL, *url = NULL;
    bool found = run_session_serving(rows, s->number, &id, &url);
    json_free(rows);
    if (!found) return false;
    set_string(&s->run_session, id);
    show_run(s, url);
    free(id); free(url);
    return true;
}
static void run_resume(PullScreen *s) { if (!s->run_url && !s->run_busy && !s->serve_error && !run_adopt(s)) run_start(s); }
/// The Run tab was opened: reads the project's run profiles once, and serves the pull request unless it is served.
static void profiles_load(PullScreen *s) {
    if (!s->profiles_read && !s->req_profiles && store_supports("projects") && store_supports("serve_pull")) store_call("projects", json_object(), 0, s, profiles_done, TAG_PROFILES, &s->req_profiles);
}
static void run_open(PullScreen *s) {
    s->tab = PR_TAB_RUN;
    profiles_load(s);
    // Until the Sessions list is read, a Run serving this pull request may be in it.
    if (store_supports("sessions") && !s->runs_read) s->run_pending = true;
    else run_resume(s);
    run_changed(s);
}
static void run_pick_profile(PullScreen *s, POINT pt) {
    if (!s->profile_count) return;
    HMENU menu = CreatePopupMenu();
    const char *shown = run_shown_profile(s);
    for (size_t i = 0; i < s->profile_count; i++) {
        char *label = i ? xstrdup(s->profiles[i]) : xstrfmt("%s (default)", s->profiles[i]);
        wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (str_eq(shown, s->profiles[i]) ? MF_CHECKED : 0), (UINT_PTR)(1 + i), wl);
        free(wl); free(label);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || (size_t)chosen > s->profile_count || str_eq(s->profiles[chosen - 1], shown)) return;
    set_string(&s->run_want, s->profiles[chosen - 1]);
    // While a request is out, its answer switches to the new pick.
    if (!s->run_busy) run_start(s);
    run_changed(s);
}
static void access_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->access_read = true;
    if (req->ok) {
        set_string(&s->access_id, json_str(json_get(req->result, "clientId")));
        set_string(&s->access_secret, json_str(json_get(req->result, "clientSecret")));
        set_string(&s->access_suffix, json_str(json_get(req->result, "hostSuffix")));
    }
    run_changed(s);
}
static void pull_place(Screen *base, const RECT *content, int scroll_y) {
    PullScreen *s = (PullScreen *)base;
    bool on = s->shown && s->tab == PR_TAB_RUN && s->run_url && !s->run_busy;
    // The browser is a child of the pane, so it starts once the tab is first shown in one, and once the service token
    // that lets it past Cloudflare Access has been read.
    if (on && !s->web && !s->access_read && store_supports("preview_access")) {
        if (!s->req_access) store_call("preview_access", json_object(), 0, s, access_done, TAG_ACCESS, &s->req_access);
        return;
    }
    if (on && !s->web) {
        WebViewAccess access = { s->access_id, s->access_secret, s->access_suffix };
        s->web = webview_new(pane_hwnd(base->pane), s->run_url, &access, web_changed, s);
    }
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
    // With the sidebar, the title and the tabs stay at the top while the tab's content scrolls beneath them, unless
    // they would take most of the view (a long stack overview).
    RECT view = pane_content_rect(base->pane);
    int view_h = view.bottom - view.top;
    bool files = s->tab == PR_TAB_FILES && !json_is_null(s->pr);
    if (s->tab != PR_TAB_RUN && !files && doc->y < view_h / 2) doc_pin(doc, (int)doc->count, doc->y);
    doc_space(doc, px(18));
    if (s->tab == PR_TAB_RUN) {
        layout_run(s, doc, w);
        return;
    } else if (files) {
        // The files take the whole width: GitHub's Files changed tab has no sidebar.
        layout_main(s, doc, col_make(0, w));
    } else if (w >= px(880)) {
        // Wide: the conversation on the left, GitHub's sidebar on the right. The sidebar stays in view beside the
        // conversation, and scrolls on its own when it is taller than the view.
        int side_w = w * 26 / 100, gap = px(28);
        if (side_w < px(240)) side_w = px(240);
        if (side_w > px(320)) side_w = px(320);
        Col main = col_make(0, w - side_w - gap), side = { w - side_w, side_w, w - side_w, side_w };
        int top = doc->y;
        layout_main(s, doc, main);
        int main_bottom = doc->y;
        int side_top = doc->y = top - px(14), side_first = (int)doc->count;
        layout_sidebar(s, doc, side);
        if (doc->pin_last) {
            // The page is as long as the conversation, or as the sidebar's window when that is longer.
            int side_h = doc->y - side_top, room = view_h - side_top - px(12);
            if (side_h > room) side_h = room;
            doc->y = side_top + side_h;
            if (doc->y < main_bottom) doc->y = main_bottom;
            doc_sticky(doc, side_first, (int)doc->count, doc->y);
        } else if (doc->y < main_bottom) doc->y = main_bottom;
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
        // The pull request stays shown; the new session joins its runs.
        if (store_supports("sessions")) { request_cancel(&s->req_sessions); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, sessions_done_pull, TAG_SESSIONS, &s->req_sessions); }
    } else {
        request_error_into(&s->write_error, req);
        // A refusal is definite; anything else may have started the session.
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}
static void start_action(PullScreen *s, const BoardAction *action, const char *input) {
    const char *branch = json_str(json_get(s->pr, "headRef"));
    if (s->busy || s->uncertain || !branch) return;
    s->busy = true;
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
        if (!request_outcome_unknown(req)) set_string(&s->merge_error, t);
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
static void edit_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->editing = false;
    if (req->ok) {
        set_string(&s->edit_error, NULL);
        // The title and description show as saved at once; the labels and assignees come back with the board's row.
        const Json *pr = json_get(req->result, "pr");
        const char *title = json_str(json_get(pr, "title")), *body = json_str(json_get(pr, "body"));
        if (title && json_is_object(s->pr)) json_set_str(s->pr, "title", title);
        if (body && json_get(req->args, "body")) { set_string(&s->body, body); if (json_str(json_get(s->pr, "body"))) json_set_str(s->pr, "body", body); }
        save_pull(s);
    } else request_error_into(&s->edit_error, req);   // an edit sets what it names, so trying again is safe
    request_cancel(&s->req_pull); pull_load(s);
    pane_relayout(s->base.pane);
}
/// Sends `fields` (taken) as an edit of this pull request.
static void pull_edit(PullScreen *s, Json *fields) {
    if (!fields) return;
    if (s->editing) { json_free(fields); return; }
    s->editing = true; set_string(&s->edit_error, NULL);
    json_set_str(fields, "repo", s->project.repo); json_set_num(fields, "pr", s->number);
    store_call("update_pull", fields, 0, s, edit_done, TAG_EDIT, &s->req_edit);
    pane_relayout(s->base.pane);
}
static void update_branch_done(void *owner, Request *req) {
    PullScreen *s = owner;
    s->updating_branch = false;
    if (req->ok) {
        set_string(&s->merge_error, NULL);
        const char *base_ref = json_str(json_get(req->args, "baseRef"));
        char *note = xstrfmt("GitHub is merging %s into this branch; its new commit and checks show once it has.", base_ref ? base_ref : "the base");
        set_string(&s->branch_note, note); free(note);
    } else {
        set_string(&s->branch_note, NULL);
        char *t = request_error_text(req);
        if (!request_outcome_unknown(req)) set_string(&s->merge_error, t);
        else { char *m = xstrfmt("%s The update may still have been made; refresh before trying again.", t); set_string(&s->merge_error, m); free(m); }
        free(t);
    }
    request_cancel(&s->req_pull); pull_load(s);
    pane_relayout(s->base.pane);
}
/// Merges the base into the branch on GitHub, at the head and base last read, once asked.
static void update_branch(PullScreen *s) {
    const char *head = json_str(json_get(s->pr, "headSha")), *base_ref = json_str(json_get(s->pr, "baseRef")), *head_ref = json_str(json_get(s->pr, "headRef"));
    if (s->updating_branch || s->merging || !head || !base_ref) return;
    char *title = xstrfmt("Update the branch of #%d?", s->number);
    char *message = xstrfmt("GitHub merges the latest changes from %s into %s, as a new commit on the branch. A session working on it needs to pull before it pushes again.", base_ref, head_ref ? head_ref : "its branch");
    s->dialog_open = true;
    bool ok = app_confirm(title, message, "Update branch", false);
    s->dialog_open = false;
    free(title); free(message);
    // The pull request may have been read again while the dialog was open.
    head = json_str(json_get(s->pr, "headSha")); base_ref = json_str(json_get(s->pr, "baseRef"));
    if (!ok || s->updating_branch || !head || !base_ref) return;
    s->updating_branch = true; set_string(&s->merge_error, NULL); set_string(&s->branch_note, NULL);
    Json *args = json_object();
    json_set_str(args, "repo", s->project.repo); json_set_num(args, "pr", s->number);
    json_set_str(args, "headSha", head); json_set_str(args, "baseRef", base_ref);
    store_call("update_pull_branch", args, 0, s, update_branch_done, TAG_UPDATE_BRANCH, &s->req_update_branch);
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
        if (arg == PR_TAB_RUN) { run_open(s); break; }
        s->tab = (int)arg;
        if (s->tab == PR_TAB_FILES) pull_files_load(s->files);
        if (s->tab == PR_TAB_CONVERSATION) conv_load(s);
        pane_relayout(base->pane); pane_header_changed(base->pane);
        break;
    case ACT_WEB_RELOAD: if (s->web) webview_reload(s->web); break;
    case ACT_RUN_PROFILE: run_pick_profile(s, pt); break;
    case ACT_RUN_RETRY: run_start(s); break;
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
        // An issue of this repository opens here, which reads the rest itself; any other on GitHub.
        const PullSummary *row = board_row(s);
        BoardLink parsed; memset(&parsed, 0, sizeof parsed);
        const BoardLink *link = row && (size_t)arg < row->issue_count ? &row->issues[arg]
            : board_link_parse(json_at(json_get(s->pr, "issues"), (size_t)arg), &parsed) ? &parsed : NULL;
        if (link && !board_link_is_foreign(link, s->project.repo) && store_supports("issue")) {
            IssueSummary bare; memset(&bare, 0, sizeof bare);
            bare.number = link->number; bare.title = link->title; bare.url = link->url;
            app_push_detail(issue_detail_screen_new(&s->project, &bare));
        } else if (link && safe_web_url(link->url)) open_web_url(link->url);
        board_link_free(&parsed);
        break;
    }
    case ACT_PR_PROJECT: open_web_url(json_str(json_get(json_at(json_get(json_at(s->issue_projects, (size_t)(arg >> 16)), "projects"), (size_t)(arg & 0xFFFF)), "url"))); break;
    case ACT_MERGE: merge(s, "squash"); break;
    case ACT_UPDATE_BRANCH: update_branch(s); break;
    case ACT_EDIT: {
        const char *title = json_str(json_get(s->pr, "title"));
        if (!title && board_row(s)) title = board_row(s)->title;
        if (s->editing || !pull_body(s)) break;
        char *t = xstrdup(title ? title : ""), *b = xstrdup(pull_body(s));
        s->dialog_open = true;
        Json *fields = details_edit("pull request", s->number, t, b);
        s->dialog_open = false;
        free(t); free(b);
        pull_edit(s, fields);
        break;
    }
    case ACT_EDIT_ASSIGNEES: case ACT_EDIT_LABELS: {
        const PullSummary *row = board_row(s);
        if (!row || s->editing) break;
        s->dialog_open = true;
        Json *list = action == ACT_EDIT_LABELS ? labels_edit(row->labels, row->label_count, s->number)
                                               : assignees_edit(pane_hwnd(base->pane), pt, row->assignees, row->assignee_count, s->number);
        s->dialog_open = false;
        if (list) { Json *fields = json_object(); json_object_set(fields, action == ACT_EDIT_LABELS ? "labels" : "assignees", list); pull_edit(s, fields); }
        pane_relayout(base->pane);
        break;
    }
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
    case ACT_DELETE_SERVED: {
        if (!run_target(s) || s->deleting_run || !store_supports("delete")) break;
        char *id = xstrdup(run_target(s));
        s->dialog_open = true;
        bool ok = app_confirm("Delete this run?", "Its workspace stops serving the pull request, and its conversation and transcript are deleted permanently.", "Delete", true);
        s->dialog_open = false;
        // The run may have changed while the dialog was open: delete the one asked about only if it is still shown.
        if (ok && !s->deleting_run && str_eq(id, run_target(s))) {
            set_string(&s->deleting_run, id); set_string(&s->run_error, NULL);
            Json *args = json_object(); json_set_str(args, "sessionId", id);
            store_call("delete", args, 0, s, delete_run_done, TAG_DELETE_RUN, &s->req_delete_run);
            pane_relayout(s->base.pane);
        }
        free(id);
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
    if (id == TIMER_RUN_LOG) { run_log_tick(s); return; }
    if (poller_fired(&s->poller, id)) {
        bool enabled = !s->busy && !s->merging && !s->deciding && !s->editing && !s->updating_branch && !s->dialog_open;
        if (enabled) pull_load(s); else poller_finished(&s->poller, false, -1);
    }
}
static void profiles_load(PullScreen *s);
static void pull_visible(Screen *base, bool shown) {
    PullScreen *s = (PullScreen *)base;
    s->shown = shown;
    if (s->web && !shown) webview_show(s->web, false);
    if (shown) profiles_load(s);
    if (shown && s->run_busy) SetTimer(pane_hwnd(base->pane), TIMER_RUN_LOG, 1500, NULL);
    if (!shown) KillTimer(pane_hwnd(base->pane), TIMER_RUN_LOG);
    if (shown) { poller_start(&s->poller, base->pane, TIMER_POLL, 30000); if (s->tab == PR_TAB_FILES) pull_files_load(s->files); if (s->tab == PR_TAB_CONVERSATION) conv_load(s); }
    else { poller_stop(&s->poller); pull_files_cancel(s->files); conv_cancel(s); request_cancel(&s->req_pull); request_cancel(&s->req_findings); request_cancel(&s->req_rows); request_cancel(&s->req_actions); request_cancel(&s->req_sessions); request_cancel(&s->req_body); request_cancel(&s->req_issue); }
}
static void pull_refresh(Screen *base) {
    PullScreen *s = (PullScreen *)base;
    // F5 on the Run tab reloads the page, as in a browser.
    if (s->tab == PR_TAB_RUN && s->web) { webview_reload(s->web); return; }
    // Refreshing is how an uncertain start is checked: its conversation is listed in the Sessions tab if it began.
    s->uncertain = false; set_string(&s->write_error, NULL); set_string(&s->edit_error, NULL); set_string(&s->branch_note, NULL);
    request_cancel(&s->req_body); s->body_read = false;
    request_cancel(&s->req_issue); s->issues_read = false; json_free(s->issue_incoming); s->issue_incoming = NULL;
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

static void board_open_run(PullsScreen *s, size_t index) {
    const PullSummary *pull = &s->pulls[index];
    StackPosition stack; bool has_stack = stack_position_parse(json_get(pull->raw, "stack"), json_get(s->board, "stacks"), &stack);
    Screen *screen = pull_detail_screen_new(&s->project, pull->number, has_stack ? &stack : NULL, pull);
    if (has_stack) stack_position_free(&stack);
    run_open((PullScreen *)screen);
    app_push_detail(screen);
}

// MARK: - Issue

enum { ACT_ISSUE_OPEN = 1200, ACT_ISSUE_PARENT, ACT_ISSUE_PULL, ACT_ISSUE_START, ACT_ISSUE_CHECKED, ACT_ISSUE_SUB, ACT_ISSUE_RUN, ACT_ISSUE_COPY, ACT_ISSUE_REFRESH, ACT_ISSUE_CLOSE,
       ACT_ISSUE_SUB_LINK, ACT_ISSUE_EVENT, ACT_ISSUE_PROJECT, ACT_ISSUE_MORE, ACT_ISSUE_EDIT, ACT_ISSUE_ASSIGNEES, ACT_ISSUE_LABELS };

// The issue as GitHub has it (`issue`): its body, type, projects, every sub-issue and linked pull request, and its
// timeline a page at a time. The board is still read beside it, on a timer, for the rows of the open sub-issues and
// pull requests, which carry their checks and reviews. On a server without the issue's own read, the board's row is
// all the screen has.
typedef struct {
    Screen base;
    Project project; IssueSummary issue;
    IssueSummary *board_issues; size_t board_issue_count;
    PullSummary *board_pulls; size_t board_pull_count;
    bool board_read, gone;      // gone: the board was read and this issue is not on it
    char *load_error, *detail_error;
    Json *detail;                        // the issue in full; NULL until read
    BoardLink *subs; size_t sub_count;   // every sub-issue it lists, closed ones and other repositories' included
    Json *events;                        // the timeline pages read so far, oldest first
    int next_page;                       // the timeline's next page; 0 once it is all read
    bool timeline_read;
    char *timeline_error;
    char *timeline_seen, *timeline_want; // the issue's updatedAt the timeline was read at, and is being read at
    Session *runs; size_t run_count;   // the conversations started on this issue or on a pull request closing it
    bool busy, uncertain;
    char *write_error, *edit_error;   // edit_error is its own, so an edit leaves an uncertain start's notice up
    bool closing, closed;           // closed: this screen closed it; the board no longer lists it
    char *closed_reason;            // completed or not_planned, as the server answered
    bool editing;                   // its title, description, labels or assignees are being saved
    Request *req, *req_board, *req_runs, *req_close, *req_detail, *req_timeline, *req_edit;
    Poller poller;
} IssueScreen;

static void issue_subs_free(IssueScreen *s) {
    for (size_t i = 0; i < s->sub_count; i++) board_link_free(&s->subs[i]);
    free(s->subs); s->subs = NULL; s->sub_count = 0;
}
static void issue_destroy(Screen *base) {
    IssueScreen *s = (IssueScreen *)base;
    request_cancel(&s->req); request_cancel(&s->req_board); request_cancel(&s->req_runs); request_cancel(&s->req_close);
    request_cancel(&s->req_detail); request_cancel(&s->req_timeline); poller_stop(&s->poller);
    request_cancel(&s->req_edit); free(s->closed_reason);
    issue_summaries_free(s->board_issues, s->board_issue_count); pull_summaries_free(s->board_pulls, s->board_pull_count);
    sessions_free(s->runs, s->run_count);
    json_free(s->detail); json_free(s->events); issue_subs_free(s);
    free(s->timeline_error); free(s->timeline_seen); free(s->timeline_want); free(s->detail_error);
    project_free(&s->project); issue_summary_free(&s->issue); free(s->write_error); free(s->edit_error); free(s->load_error);
    screen_release(base);
}
/// Takes the board's answer: its rows, and this issue's own row when it is still there and its full read is not.
/// A saved board only fills in.
static void issue_board_show(IssueScreen *s, const Json *board, bool saved) {
    issue_summaries_free(s->board_issues, s->board_issue_count); s->board_issues = issue_summaries_parse(json_get(board, "issues"), &s->board_issue_count);
    pull_summaries_free(s->board_pulls, s->board_pull_count); s->board_pulls = pull_summaries_parse(json_get(board, "pulls"), &s->board_pull_count);
    const Json *rows = json_get(board, "issues");
    bool found = false;
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        double n;
        if (!json_num(json_get(row, "number"), &n) || (int)n != s->issue.number) continue;
        IssueSummary fresh;
        found = true;
        if (!saved && !s->detail && issue_summary_parse(row, &fresh)) { issue_summary_free(&s->issue); s->issue = fresh; }
        break;
    }
    // A board GitHub refused the issues of says nothing about this one.
    if (!saved) s->gone = !found && !json_is_set(json_get(board, "issuesError"));
    s->board_read = true;
}
/// Takes the issue's own read: the row it shares with the board, and all the board does not carry.
static void issue_detail_show(IssueScreen *s, const Json *issue) {
    IssueSummary fresh;
    if (!issue_summary_parse(issue, &fresh)) return;
    if (fresh.number != s->issue.number) { issue_summary_free(&fresh); return; }
    issue_summary_free(&s->issue); s->issue = fresh;
    json_free(s->detail); s->detail = json_clone(issue);
    issue_subs_free(s);
    const Json *items = json_get(json_get(issue, "subIssues"), "items");
    s->subs = xcalloc(json_count(items) ? json_count(items) : 1, sizeof *s->subs);
    for (size_t i = 0; i < json_count(items); i++) if (board_link_parse(json_at(items, i), &s->subs[s->sub_count])) s->sub_count++;
}
static void issue_timeline_show(IssueScreen *s, const Json *result, bool first) {
    if (first || !s->events) { json_free(s->events); s->events = json_array(); }
    const Json *events = json_get(result, "events");
    for (size_t i = 0; i < json_count(events); i++) json_array_push(s->events, json_clone(json_at(events, i)));
    s->next_page = json_int_or(json_get(result, "nextPage"), 0);
}
static char *issue_cache_key(const IssueScreen *s, const char *what) { return xstrfmt("%s:%s#%d", what, s->project.repo, s->issue.number); }
static void issue_timeline_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (!req->ok) request_error_into(&s->timeline_error, req);
    else {
        bool first = json_int_or(json_get(req->args, "page"), 1) <= 1;
        issue_timeline_show(s, req->result, first);
        if (first) {
            set_string(&s->timeline_seen, s->timeline_want);
            char *key = issue_cache_key(s, "issue-timeline"); cache_store(g_store.cache, req->result, key); free(key);
        }
        s->timeline_read = true;
        set_string(&s->timeline_error, NULL);
    }
    pane_relayout(s->base.pane);
}
static void issue_timeline_load(IssueScreen *s, int page) {
    if (!store_supports("issue_timeline")) return;
    request_cancel(&s->req_timeline);
    Json *a = json_object();
    json_set_num(a, "issue", s->issue.number); json_set_str(a, "repo", s->project.repo); json_set_num(a, "page", page);
    store_call("issue_timeline", a, 0, s, issue_timeline_done, 0, &s->req_timeline);
}
static void issue_detail_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (!req->ok) request_error_into(&s->detail_error, req);
    else {
        issue_detail_show(s, json_get(req->result, "issue"));
        set_string(&s->detail_error, NULL);
        char *key = issue_cache_key(s, "issue"); cache_store(g_store.cache, req->result, key); free(key);
        // The timeline is read again only when the issue has moved since; the later pages read stay until then.
        const char *updated = json_str(json_get(s->detail, "updatedAt"));
        if (!s->timeline_read || !updated || !str_eq(updated, s->timeline_seen)) { set_string(&s->timeline_want, updated); issue_timeline_load(s, 1); }
    }
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static bool issue_run_matches(const IssueScreen *s, const Session *run) {
    if (!str_eq(session_repo(run), s->project.repo)) return false;
    if (session_on_issue(run, s->issue.number)) return true;
    int pr = session_pull_number(run);
    for (size_t i = 0; pr && i < s->issue.pull_count; i++)
        if (s->issue.pulls[i].number == pr && !board_link_is_foreign(&s->issue.pulls[i], s->project.repo)) return true;
    return false;
}
static void issue_runs_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (!req->ok) return;
    Session *all; size_t n;
    if (!sessions_parse(req->result, &all, &n)) return;
    sessions_free(s->runs, s->run_count); s->runs = xcalloc(n ? n : 1, sizeof *s->runs); s->run_count = 0;
    for (size_t i = 0; i < n; i++) if (issue_run_matches(s, &all[i])) session_copy(&s->runs[s->run_count++], &all[i]);
    sessions_free(all, n);
    pane_relayout(s->base.pane);
}
static void issue_load(IssueScreen *s, bool fresh);
static void issue_board_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    if (!req->ok) {
        if (req->error.kind == API_HTTP && req->error.status == 400 && !json_is_null(json_get(req->args, "fresh"))) { issue_load(s, false); return; }
        request_error_into(&s->load_error, req);
    } else {
        issue_board_show(s, req->result, false);
        set_string(&s->load_error, NULL);
        char *key = xstrfmt("pulls:%s", s->project.repo);
        cache_store(g_store.cache, req->result, key);
        free(key);
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void issue_load(IssueScreen *s, bool fresh) {
    if (store_supports("pulls")) {
        request_cancel(&s->req_board);
        Json *args = json_object(); json_set_str(args, "repo", s->project.repo);
        if (fresh) json_set_str(args, "fresh", "1");
        store_call("pulls", args, 0, s, issue_board_done, 0, &s->req_board);
    } else poller_finished(&s->poller, false, -1);
    if (store_supports("issue") && !s->req_detail) {
        Json *a = json_object(); json_set_num(a, "issue", s->issue.number); json_set_str(a, "repo", s->project.repo);
        store_call("issue", a, 0, s, issue_detail_done, 0, &s->req_detail);
    }
    if (store_supports("sessions") && !s->req_runs) { Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, issue_runs_done, 0, &s->req_runs); }
}
static bool issue_run_active(const IssueScreen *s) { for (size_t i = 0; i < s->run_count; i++) if (session_is_active(&s->runs[i])) return true; return false; }
/// What the conversations on this issue spent, each counted with the workers it ordered. False when none was priced.
static bool issue_runs_cost(const IssueScreen *s, double *total) {
    bool any = false; *total = 0;
    for (size_t i = 0; i < s->run_count; i++) {
        double c;
        if (json_num(json_get(json_get(s->runs[i].raw, "usage"), "costUsd"), &c) || json_num(json_get(s->runs[i].raw, "costUsd"), &c)) { *total += c; any = true; }
    }
    return any;
}
/// The board row of a closing pull request of this repository, which carries its checks and reviews.
static const PullSummary *issue_pull_row(const IssueScreen *s, size_t i) {
    const BoardLink *pull = &s->issue.pulls[i];
    if (board_link_is_foreign(pull, s->project.repo)) return NULL;
    return pulls_find(s->board_pulls, s->board_pull_count, pull->number);
}
/// The pull requests still open among those linked to it; the full read lists merged and closed ones too.
static size_t issue_open_pulls(const IssueScreen *s) {
    size_t n = 0;
    for (size_t i = 0; i < s->issue.pull_count; i++) if (!s->issue.pulls[i].state || str_eq(s->issue.pulls[i].state, "open")) n++;
    return n;
}
/// An issue of this repository opens here, from its board row when it has one; any other on GitHub.
static void issue_open_link(IssueScreen *s, const BoardLink *link) {
    if (!board_link_is_foreign(link, s->project.repo)) {
        const IssueSummary *row = issues_find(s->board_issues, s->board_issue_count, link->number);
        if (row) { app_push_detail(issue_detail_screen_new(&s->project, row)); return; }
        if (store_supports("issue")) {
            IssueSummary bare; memset(&bare, 0, sizeof bare);
            bare.number = link->number; bare.title = link->title; bare.url = link->url;
            app_push_detail(issue_detail_screen_new(&s->project, &bare));
            return;
        }
    }
    open_web_url(link->url);
}
static void issue_open_pull(IssueScreen *s, const BoardLink *link) {
    if (board_link_is_foreign(link, s->project.repo) || !store_supports("pull")) open_web_url(link->url);
    else app_push_detail(pull_detail_screen_new(&s->project, link->number, NULL, pulls_find(s->board_pulls, s->board_pull_count, link->number)));
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
static bool issue_is_closed(const IssueScreen *s) { COLORREF c; return str_eq(issue_state(s, &c), "closed"); }

static bool issue_editable(const IssueScreen *s) { return store_supports("update_issue") && !s->closed; }
/// The title with its number, then the state pill and GitHub's sentence beside it: who opened it, when, its comments.
static void issue_layout_header(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    ButtonSpec buttons[2]; size_t bn = 0;
    // The description is the issue's own read; without it an edit would start from nothing.
    if (issue_editable(s)) { ButtonSpec b = { 0, s->editing ? "Saving\xE2\x80\xA6" : "Edit", BUTTON_BORDERED, ACT_ISSUE_EDIT, 0, !s->editing && json_str(json_get(s->detail, "body")) }; buttons[bn++] = b; }
    if (safe_web_url(issue->url)) { ButtonSpec b = { 0, "Open in GitHub \xE2\x86\x97", BUTTON_BORDERED, ACT_ISSUE_OPEN, 0, true }; buttons[bn++] = b; }
    int tw = bn ? toolbar_width(doc->cv, buttons, bn) + px(2) : 0;
    bool beside = !bn || c.iw - tw - px(16) >= px(320);
    int top = doc->y, buttons_bottom = top;
    if (bn && beside) { doc->y = top + px(3); doc_button_row(doc, c.ix + c.iw - tw, tw, buttons, bn); buttons_bottom = doc->y; doc->y = top; }
    else if (bn) { doc_button_row(doc, c.ix, c.iw, buttons, bn); doc_space(doc, px(12)); }
    FlowData *t = flow_new(font_height(doc->cv, FONT_TITLE) + px(6));
    char *spaced = xstrfmt("%s ", issue->title); flow_add(t, spaced, FONT_TITLE, theme.ink, false); free(spaced);
    char *number = xstrfmt("#%d", issue->number); flow_add(t, number, FONT_TITLE, theme.muted, false); free(number);
    doc_flow(doc, c.ix, bn && beside ? c.iw - tw - px(16) : c.iw, 0, t);
    if (doc->y < buttons_bottom) doc->y = buttons_bottom;
    doc_space(doc, px(10));
    COLORREF color;
    const char *state = issue_state(s, &color);
    int y = doc->y, pw = doc_pill(doc, c.ix, y, state, color);
    FlowData *f = flow_new(px(26));
    if (issue->author) { char *a = xstrfmt("%s ", issue->author); flow_add(f, a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(a); }
    if (issue->has_created) {
        char *rel = format_relative(issue->created_at), *o = xstrfmt("%s %s ", issue->author ? "opened this issue" : "Opened", rel);
        flow_add(f, o, FONT_FOOTNOTE, theme.muted, false);
        free(o); free(rel);
    }
    time_t closed_at;
    if (str_eq(state, "closed") && board_date_parse(json_str(json_get(s->detail, "closedAt")), &closed_at)) {
        char *rel = format_relative(closed_at), *t2 = xstrfmt("\xC2\xB7 closed %s ", rel);
        flow_add(f, t2, FONT_FOOTNOTE, theme.muted, false);
        free(t2); free(rel);
    }
    if (issue->comments > 0) { char *n = xstrfmt("\xC2\xB7 %d comment%s", issue->comments, issue->comments == 1 ? "" : "s"); flow_add(f, n, FONT_FOOTNOTE, theme.muted, false); free(n); }
    if (f->count) doc_flow(doc, c.ix + pw + px(10), c.iw - pw - px(10), 0, f); else flow_free(f);
    if (doc->y < y + px(26)) doc->y = y + px(26);
}
/// The opening comment: the issue's body as GitHub shows it.
static void issue_layout_body(IssueScreen *s, Doc *doc, Col c) {
    if (!store_supports("issue")) return;
    const IssueSummary *issue = &s->issue;
    CommentHeadData *h = xcalloc(1, sizeof *h);
    if (issue->author) h->author = xstrdup(issue->author);
    if (issue->has_created) { char *rel = format_relative(issue->created_at); h->when = xstrfmt("%s \xC2\xB7 %s", issue->author ? "opened" : "Description", rel); free(rel); }
    else h->when = xstrdup(issue->author ? "opened" : "Description");
    int ix, iw, box = comment_begin(doc, c, h, 0, 0, &ix, &iw);
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
static void issue_layout_subs(IssueScreen *s, Doc *doc, Col c) {
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
        int box = col_box(doc, c);
        for (size_t k = 0; k < rn; k++) {
            const BoardLink *link = &s->subs[rest[k]];
            if (k) doc_space(doc, px(4));
            doc_linked_row(doc, c.ix, c.iw, link, s->project.repo, safe_web_url(link->url) || store_supports("issue") ? ACT_ISSUE_SUB_LINK : 0, (intptr_t)rest[k]);
        }
        doc_box_end(doc, box, px(12));
    } else if (!sn) {
        int box = col_box(doc, c);
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
/// The pull requests linked to close it: those on the board drawn as the board draws them, the rest as links.
static void issue_layout_pulls(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    doc_section(doc, c.x, c.w, "Pull requests");
    bool thin = false;
    for (size_t i = 0; i < issue->pull_count; i++) {
        const PullSummary *row = issue_pull_row(s, i);
        if (!row) { thin = true; continue; }
        bool running = false;
        for (size_t r = 0; r < s->run_count; r++) if (session_pull_number(&s->runs[r]) == row->number && session_is_active(&s->runs[r])) running = true;
        // The issues it closes would only name this one again.
        PullSummary shown = *row; shown.issue_count = 0;
        doc_pull_row(doc, c.x, c.w, &shown, NULL, s->project.repo, NULL, store_supports("pull") || safe_web_url(row->url) ? ACT_ISSUE_PULL : 0, (intptr_t)i, NULL, 0, running);
        doc_space(doc, px(8));
    }
    if (thin || !issue->pull_count) {
        int box = col_box(doc, c);
        bool first = true;
        for (size_t i = 0; i < issue->pull_count; i++) {
            if (issue_pull_row(s, i)) continue;
            if (!first) doc_space(doc, px(4));
            first = false;
            const BoardLink *pull = &issue->pulls[i];
            bool foreign = board_link_is_foreign(pull, s->project.repo) || !store_supports("pull");
            doc_linked_row(doc, c.ix, c.iw, pull, s->project.repo, foreign ? (safe_web_url(pull->url) ? ACT_ISSUE_PULL : 0) : ACT_ISSUE_PULL, (intptr_t)i);
        }
        if (!issue->pull_count) doc_text(doc, c.ix, c.iw, s->detail ? "No pull request is linked to close this issue yet" : "No open pull request closes this issue yet", FONT_CALLOUT, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, box, px(12));
    }
}
static void issue_layout_runs(IssueScreen *s, Doc *doc, Col c) {
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

// The timeline: comments in boxes as GitHub draws them, and every other event a line beside a round badge.

typedef struct { wchar_t glyph; COLORREF color; } EventBadge;
static void paint_event_badge(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    EventBadge *d = it->data;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, theme.raise, theme.line);
    draw_glyph(cv, d->glyph, rc, FONT_ICON_SMALL, d->color);
}
static void flow_strong(FlowData *f, const char *text) { if (text) { char *t = xstrfmt("%s ", text); flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(t); } }
static void flow_muted(FlowData *f, const char *text) { flow_add(f, text, FONT_FOOTNOTE, theme.muted, false); }
/// An issue or pull request an event points to: `#12 Its title`, or `owner/name#12` from another repository.
static void flow_ref(FlowData *f, const Json *ref, const char *repo) {
    BoardLink link;
    if (!board_link_parse(ref, &link)) return;
    char *r = board_link_reference(&link, repo), *t = xstrfmt("%s ", r);
    flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.accent, false);
    char *title = xstrfmt("%s ", link.title); flow_add(f, title, FONT_FOOTNOTE, theme.ink, false);
    free(title); free(t); free(r);
    board_link_free(&link);
}
/// An event as GitHub words it after its actor, with its badge. False for a kind this screen does not draw.
static bool event_words(const Json *e, const char *repo, FlowData *f, wchar_t *glyph, COLORREF *color) {
    const char *kind = json_str(json_get(e, "kind"));
    *glyph = 0xE8EC; *color = theme.muted;
    if (str_eq(kind, "labeled") || str_eq(kind, "unlabeled")) {
        const char *name = json_str(json_get(json_get(e, "label"), "name"));
        flow_muted(f, str_eq(kind, "labeled") ? "added the " : "removed the ");
        if (name) flow_add(f, name, FONT_CAPTION_SEMIBOLD, theme.accent, true);
        flow_muted(f, "label ");
    } else if (str_eq(kind, "assigned") || str_eq(kind, "unassigned")) {
        const char *who = json_str(json_get(e, "assignee"));
        bool self = str_ieq(who, json_str(json_get(e, "actor")));
        *glyph = 0xE77B;
        if (str_eq(kind, "assigned")) { if (self) flow_muted(f, "self-assigned this "); else { flow_muted(f, "assigned "); flow_strong(f, who); } }
        else if (self) flow_muted(f, "removed their assignment ");
        else { flow_muted(f, "unassigned "); flow_strong(f, who); }
    } else if (str_eq(kind, "milestoned") || str_eq(kind, "demilestoned")) {
        *glyph = 0xE7C1;
        flow_muted(f, str_eq(kind, "milestoned") ? "added this to the " : "removed this from the ");
        flow_strong(f, json_str(json_get(e, "milestone")));
        flow_muted(f, "milestone ");
    } else if (str_eq(kind, "renamed")) {
        *glyph = 0xE70F;
        const char *from = json_str(json_get(e, "from")), *to = json_str(json_get(e, "to"));
        flow_muted(f, "changed the title ");
        if (from) { char *t = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D ", from); flow_add(f, t, FONT_FOOTNOTE, theme.secondary, false); free(t); }
        flow_muted(f, "to ");
        if (to) { char *t = xstrfmt("\xE2\x80\x9C%s\xE2\x80\x9D ", to); flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(t); }
    } else if (str_eq(kind, "closed")) {
        const char *reason = json_str(json_get(e, "stateReason"));
        bool aside = str_eq(reason, "not_planned") || str_eq(reason, "duplicate");
        *glyph = aside ? 0xE711 : 0xE73E; *color = aside ? theme.muted : theme.accent;
        flow_muted(f, str_eq(reason, "not_planned") ? "closed this as not planned " : str_eq(reason, "duplicate") ? "closed this as a duplicate " : str_eq(reason, "completed") ? "closed this as completed " : "closed this ");
    } else if (str_eq(kind, "reopened")) {
        *glyph = 0xE72C; *color = theme.ok;
        flow_muted(f, "reopened this ");
    } else if (str_eq(kind, "cross-referenced")) {
        *glyph = 0xE71B;
        flow_muted(f, "mentioned this in ");
        flow_ref(f, json_get(e, "source"), repo);
    } else if (str_eq(kind, "connected") || str_eq(kind, "disconnected")) {
        *glyph = 0xE71B;
        flow_muted(f, str_eq(kind, "connected") ? "linked a pull request that will close this issue " : "removed a link to a pull request ");
        flow_ref(f, json_get(e, "source"), repo);
    } else if (str_eq(kind, "referenced")) {
        const Json *commit = json_get(e, "commit");
        const char *sha = json_str(json_get(commit, "sha")), *message = json_str(json_get(commit, "message"));
        *glyph = 0xE8EE;
        flow_muted(f, "referenced this in commit ");
        if (sha) { char *short_sha = xstrndup(sha, strlen(sha) > 7 ? 7 : strlen(sha)); flow_add(f, short_sha, FONT_MONO_CAPTION2, theme.accent, true); free(short_sha); }
        if (message) { char *m = xstrfmt("%s ", message); flow_add(f, m, FONT_FOOTNOTE, theme.ink, false); free(m); }
    } else if (str_eq(kind, "parent_issue_added") || str_eq(kind, "parent_issue_removed")) {
        *glyph = 0xE71B;
        flow_muted(f, str_eq(kind, "parent_issue_added") ? "added a parent issue " : "removed a parent issue ");
        flow_ref(f, json_get(e, "issue"), repo);
    } else if (str_eq(kind, "sub_issue_added") || str_eq(kind, "sub_issue_removed")) {
        *glyph = 0xE71B;
        flow_muted(f, str_eq(kind, "sub_issue_added") ? "added a sub-issue " : "removed a sub-issue ");
        flow_ref(f, json_get(e, "issue"), repo);
    } else if (str_eq(kind, "issue_type_added") || str_eq(kind, "issue_type_removed") || str_eq(kind, "issue_type_changed")) {
        const char *type = json_str(json_get(e, "type")), *before = json_str(json_get(e, "previousType"));
        if (str_eq(kind, "issue_type_changed")) { flow_muted(f, "changed the issue type from "); flow_strong(f, before); flow_muted(f, "to "); flow_strong(f, type); }
        else { flow_muted(f, str_eq(kind, "issue_type_added") ? "added the " : "removed the "); flow_strong(f, type); flow_muted(f, "issue type "); }
    } else if (str_eq(kind, "added_to_project_v2") || str_eq(kind, "removed_from_project_v2") || str_eq(kind, "project_v2_item_status_changed")) {
        const char *project = json_str(json_get(e, "project"));
        *glyph = 0xE8FD;
        if (str_eq(kind, "project_v2_item_status_changed")) {
            const char *before = json_str(json_get(e, "previousStatus")), *after = json_str(json_get(e, "status"));
            flow_muted(f, "moved this ");
            if (before) { flow_muted(f, "from "); flow_strong(f, before); }
            flow_muted(f, "to "); flow_strong(f, after ? after : "No status");
        } else flow_muted(f, str_eq(kind, "added_to_project_v2") ? "added this to " : "removed this from ");
        if (project) { if (str_eq(kind, "project_v2_item_status_changed")) flow_muted(f, "in "); flow_strong(f, project); }
        else if (!str_eq(kind, "project_v2_item_status_changed")) flow_muted(f, "a project ");
    } else return false;
    return true;
}
/// Where clicking an event goes: a comment's place on GitHub, the issue, pull request or commit it points to.
static bool event_has_target(const Json *e) {
    if (str_eq(json_str(json_get(e, "kind")), "commented")) return safe_web_url(json_str(json_get(e, "url")));
    double n;
    if (json_num(json_get(json_get(e, "source"), "number"), &n) || json_num(json_get(json_get(e, "issue"), "number"), &n)) return true;
    return safe_web_url(json_str(json_get(json_get(e, "commit"), "url")));
}
static void event_open(IssueScreen *s, const Json *e) {
    if (str_eq(json_str(json_get(e, "kind")), "commented")) { open_web_url(json_str(json_get(e, "url"))); return; }
    const Json *source = json_get(e, "source"), *ref = json_is_set(source) ? source : json_get(e, "issue");
    BoardLink link;
    if (board_link_parse(ref, &link)) {
        if (str_eq(json_str(json_get(ref, "kind")), "pull")) issue_open_pull(s, &link); else issue_open_link(s, &link);
        board_link_free(&link);
        return;
    }
    open_web_url(json_str(json_get(json_get(e, "commit"), "url")));
}
static void issue_layout_activity(IssueScreen *s, Doc *doc, Col c) {
    // The timeline is read once the issue is; when that read failed, there is none coming.
    if (!store_supports("issue_timeline") || (!s->events && !s->req_timeline && s->detail_error)) return;
    doc_section(doc, c.x, c.w, "Activity");
    if (s->timeline_error) { doc_notice(doc, c.x, c.w, s->timeline_error); doc_space(doc, px(10)); }
    if (!s->events) { if (!s->timeline_error) doc_loading(doc, c.x, c.w, "Loading the timeline\xE2\x80\xA6"); return; }
    size_t drawn = 0;
    for (size_t i = 0; i < json_count(s->events); i++) {
        const Json *e = json_at(s->events, i);
        const char *actor = json_str(json_get(e, "actor"));
        time_t at; bool dated = board_date_parse(json_str(json_get(e, "createdAt")), &at);
        int action = event_has_target(e) ? ACT_ISSUE_EVENT : 0;
        if (str_eq(json_str(json_get(e, "kind")), "commented")) {
            if (drawn) doc_space(doc, px(12));
            CommentHeadData *h = xcalloc(1, sizeof *h);
            h->author = xstrdup(actor ? actor : "ghost");
            if (dated) { char *rel = format_relative(at); h->when = xstrfmt("commented \xC2\xB7 %s", rel); free(rel); }
            else h->when = xstrdup("commented");
            int ix, iw, box = comment_begin(doc, c, h, action, (intptr_t)i, &ix, &iw);
            char *body = visible_markdown(json_str(json_get(e, "body")) ? json_str(json_get(e, "body")) : "");
            if (*body) doc_markdown(doc, ix, iw, body, FONT_CALLOUT);
            else doc_text(doc, ix, iw, "No description provided.", FONT_CALLOUT_ITALIC, theme.secondary, DT_WORDBREAK);
            free(body);
            doc_box_end(doc, box, px(14));
            drawn++;
            continue;
        }
        FlowData *f = flow_new(px(22));
        flow_strong(f, actor ? actor : "ghost");
        wchar_t glyph; COLORREF color;
        if (!event_words(e, s->project.repo, f, &glyph, &color)) { flow_free(f); continue; }
        if (dated) { char *rel = format_relative(at); flow_muted(f, rel); free(rel); }
        doc_space(doc, px(drawn ? 10 : 2));
        int y = doc->y;
        EventBadge *b = xcalloc(1, sizeof *b); b->glyph = glyph; b->color = color;
        int bi = doc_custom(doc, c.x + px(14), px(26), px(26), paint_event_badge, b, free, 0, 0);
        doc_item(doc, bi)->hover_fill = false;
        doc->y = y + px(2);
        int fi = doc_flow(doc, c.x + px(50), c.w - px(50), 0, f);
        if (action) { doc_item(doc, fi)->action = action; doc_item(doc, fi)->arg = (intptr_t)i; doc_item(doc, fi)->hand = true; }
        if (doc->y < y + px(26)) doc->y = y + px(26);
        drawn++;
    }
    if (!drawn) doc_text(doc, c.x, c.w, "No activity yet", FONT_CALLOUT, theme.secondary, DT_SINGLELINE);
    if (s->next_page) {
        doc_space(doc, px(14));
        doc_button(doc, c.x, 0, s->req_timeline ? "Loading\xE2\x80\xA6" : "Show more activity", BUTTON_BORDERED, ACT_ISSUE_MORE, 0, !s->req_timeline);
    }
}
static void issue_layout_main(IssueScreen *s, Doc *doc, Col c) {
    issue_layout_body(s, doc, c);
    issue_layout_subs(s, doc, c);
    issue_layout_pulls(s, doc, c);
    issue_layout_runs(s, doc, c);
    issue_layout_activity(s, doc, c);
}

// The sidebar, as GitHub's: what can be done here, then who and what it is filed under.

static void issue_side_actions(IssueScreen *s, Doc *doc, Col c, int *count) {
    if (issue_is_closed(s)) return;
    bool start = store_supports("start_session"), close = store_supports("close_issue");
    if (!start && !close) return;
    side_heading(doc, c, count, s->busy ? "Actions \xC2\xB7 starting\xE2\x80\xA6" : "Actions");
    const IssueSummary *issue = &s->issue;
    if (start && issue_is_epic(issue)) {
        doc_text(doc, c.x, c.w, "An epic is worked by an orchestrator, one sub-issue at a time. Start it from the web dashboard, where its models are picked, or start one of its sub-issues here.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    } else if (start) {
        doc_button(doc, c.x, c.w, s->busy ? "Starting\xE2\x80\xA6" : "Start a session on this issue", BUTTON_PROMINENT, ACT_ISSUE_START, 0, !s->busy && !s->uncertain);
        doc_space(doc, px(8));
        doc_text(doc, c.x, c.w, issue_run_active(s) ? "A session is already working on this issue; it is listed under Sessions. A second one is a paid agent doing the same work."
                              : issue_open_pulls(s) ? "A pull request is already answering this issue. A second session is a paid agent working on the same thing."
                                                    : "The session reads the issue, implements it on a branch of its own and opens a pull request closing it. It runs a paid agent on this project\xE2\x80\x99s configured model.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
    if (close) {
        if (start) doc_space(doc, px(12));
        doc_button(doc, c.x, c.w, s->closing ? "Closing\xE2\x80\xA6" : "Close issue \xE2\x96\xBE", BUTTON_BORDERED, ACT_ISSUE_CLOSE, 0, !s->closing);
        doc_space(doc, px(8));
        doc_text(doc, c.x, c.w, issue_open_pulls(s) ? "Closes it on GitHub now. The pull requests answering it stay open; merging one later will not close it again."
                                                    : "Closes it on GitHub, as completed or as not planned, with a comment first if you write one.",
                 FONT_CAPTION, theme.secondary, DT_WORDBREAK);
    }
}
static void issue_side_projects(IssueScreen *s, Doc *doc, Col c, int *count) {
    if (!s->detail) return;
    const Json *projects = json_get(s->detail, "projects");
    side_heading(doc, c, count, "Projects");
    for (size_t i = 0; i < json_count(projects); i++) { if (i) doc_space(doc, px(10)); side_project(doc, c, json_at(projects, i), NULL, ACT_ISSUE_PROJECT, (intptr_t)i); }
    if (!json_count(projects)) side_projects_none(doc, c, json_str(json_get(s->detail, "projectsError")));
}
static void issue_layout_sidebar(IssueScreen *s, Doc *doc, Col c) {
    const IssueSummary *issue = &s->issue;
    int count = 0;
    issue_side_actions(s, doc, c, &count);
    side_heading(doc, c, &count, "Assignees");
    for (size_t i = 0; i < issue->assignee_count; i++) { if (i) doc_space(doc, px(4)); doc_text(doc, c.x, c.w, issue->assignees[i], FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS); }
    if (!issue->assignee_count) doc_text(doc, c.x, c.w, "No one", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (issue_editable(s)) side_edit_button(doc, c, "Edit assignees \xE2\x96\xBE", ACT_ISSUE_ASSIGNEES, !s->editing);
    side_heading(doc, c, &count, "Labels");
    if (issue->label_count) doc_label_chips(doc, c.x, c.w, issue->labels, issue->label_count, theme.canvas);
    else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (issue_editable(s)) side_edit_button(doc, c, "Edit labels\xE2\x80\xA6", ACT_ISSUE_LABELS, !s->editing);
    if (s->detail) {
        const char *type = json_str(json_get(s->detail, "type"));
        side_heading(doc, c, &count, "Type");
        doc_text(doc, c.x, c.w, type ? type : "No type", type ? FONT_FOOTNOTE : FONT_CAPTION, type ? theme.ink : theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    issue_side_projects(s, doc, c, &count);
    side_heading(doc, c, &count, "Milestone");
    doc_text(doc, c.x, c.w, issue->milestone ? issue->milestone : "No milestone", issue->milestone ? FONT_FOOTNOTE : FONT_CAPTION, issue->milestone ? theme.ink : theme.secondary, DT_WORDBREAK);
    side_heading(doc, c, &count, "Relationships");
    if (issue->has_parent) {
        doc_text(doc, c.x, c.w, "Parent issue", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
        doc_space(doc, px(2));
        bool linked = !board_link_is_foreign(&issue->parent, s->project.repo) || safe_web_url(issue->parent.url);
        doc_linked_row(doc, c.x, c.w, &issue->parent, s->project.repo, linked ? ACT_ISSUE_PARENT : 0, 0);
    } else doc_text(doc, c.x, c.w, "None yet", FONT_CAPTION, theme.secondary, DT_SINGLELINE);
    if (issue->has_updated) {
        side_heading(doc, c, &count, "Updated");
        char *rel = format_relative(issue->updated_at); doc_text(doc, c.x, c.w, rel, FONT_FOOTNOTE, theme.ink, DT_SINGLELINE); free(rel);
    }
}
static void issue_layout(Screen *base, Doc *doc) {
    IssueScreen *s = (IssueScreen *)base;
    int w = doc->width, ix = px(12), iw = w - px(24);
    doc_space(doc, px(14));
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
    if (s->edit_error) { doc_notice(doc, 0, w, s->edit_error); doc_space(doc, px(10)); }
    if (s->detail_error) { doc_notice(doc, 0, w, s->detail_error); doc_space(doc, px(10)); }
    if (s->load_error && !str_eq(s->load_error, s->detail_error)) { doc_notice(doc, 0, w, s->load_error); doc_space(doc, px(10)); }
    if (s->closed) {
        int b = section_box(doc, w);
        doc_label(doc, ix, iw, 0xE73E, str_eq(s->closed_reason, "not_planned") ? "Closed as not planned" : "Closed as completed", FONT_CALLOUT, theme.success);
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "It has left the board. Reopen it on GitHub if it was closed by mistake.", FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        doc_box_end(doc, b, px(12));
        doc_space(doc, px(10));
    } else if (s->gone && !s->detail) {
        int b = section_box(doc, w);
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
        Col main = col_make(0, w - side_w - gap), side = { w - side_w, side_w, w - side_w, side_w };
        int top = doc->y;
        issue_layout_main(s, doc, main);
        int main_bottom = doc->y;
        doc->y = top - px(14);
        issue_layout_sidebar(s, doc, side);
        if (doc->y < main_bottom) doc->y = main_bottom;
    } else {
        issue_layout_main(s, doc, col_make(0, w));
        doc_space(doc, px(6));
        Col side = { px(4), w - px(8), px(4), w - px(8) };
        issue_layout_sidebar(s, doc, side);
    }
    doc_space(doc, px(16));
}
static void issue_header(Screen *base, HeaderInfo *info) {
    IssueScreen *s = (IssueScreen *)base;
    snprintf(info->title, sizeof info->title, "#%d", s->issue.number);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", s->project.repo);
    HeaderButton *b = &info->buttons[info->button_count++]; b->glyph = 0xE8C8; b->action = ACT_ISSUE_COPY; b->enabled = safe_web_url(s->issue.url); b->tip = "Copy the issue\xE2\x80\x99s link";
    b = &info->buttons[info->button_count++]; b->glyph = 0xE72C; b->action = ACT_ISSUE_REFRESH; b->enabled = !s->req_board && !s->req_detail; b->tip = "Read the issue from GitHub again";
}
static void issue_start_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->busy = false;
    if (req->ok) {
        // The new conversation joins the Sessions list on the way back.
        if (store_supports("sessions")) { request_cancel(&s->req_runs); Json *a = json_object(); json_set_str(a, "repo", s->project.repo); store_call("sessions", a, 0, s, issue_runs_done, 0, &s->req_runs); }
        Session started;
        if (session_parse(json_get(req->result, "session"), &started)) { pane_relayout(s->base.pane); app_push_detail(conversation_screen_new(&started)); session_free(&started); return; }
    } else {
        request_error_into(&s->write_error, req);
        if (request_outcome_unknown(req)) s->uncertain = true;
    }
    pane_relayout(s->base.pane);
}
static void issue_close_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->closing = false;
    if (req->ok) {
        s->closed = true; s->gone = false;
        const char *reason = json_str(json_get(json_get(req->result, "issue"), "stateReason"));
        set_string(&s->closed_reason, reason ? reason : json_str(json_get(req->args, "reason")));
        set_string(&s->write_error, NULL);
        // The board drops it; reading it again keeps the epic's counts and its siblings true.
        issue_load(s, true);
    } else request_error_into(&s->write_error, req);   // closing twice only restates the reason, so trying again is safe
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void issue_edit_done(void *owner, Request *req) {
    IssueScreen *s = owner;
    s->editing = false;
    if (req->ok) {
        set_string(&s->edit_error, NULL);
        // The title and description show as saved at once; reading it again brings the rest.
        const Json *issue = json_get(req->result, "issue");
        const char *title = json_str(json_get(issue, "title")), *body = json_str(json_get(issue, "body"));
        if (title) set_string(&s->issue.title, title);
        if (body && s->detail && json_get(req->args, "body")) json_set_str(s->detail, "body", body);
        request_cancel(&s->req_detail);   // one already under way may have been read before the edit
        issue_load(s, true);
    } else request_error_into(&s->edit_error, req);   // an edit sets what it names, so trying again is safe
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
/// Sends `fields` (taken) as an edit of this issue.
static void issue_edit(IssueScreen *s, Json *fields) {
    if (!fields) return;
    if (s->editing) { json_free(fields); return; }
    s->editing = true; set_string(&s->edit_error, NULL);
    json_set_num(fields, "issue", s->issue.number); json_set_str(fields, "repo", s->project.repo);
    store_call("update_issue", fields, 0, s, issue_edit_done, 0, &s->req_edit);
    pane_relayout(s->base.pane);
}
/// The ▾ menu: why it is closed, and whether a comment goes first. Then a confirmation naming what stays open.
static void issue_close(IssueScreen *s, POINT pt) {
    if (s->closing || s->closed) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Close as completed");
    AppendMenuW(menu, MF_STRING, 2, L"Close as not planned");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, 3, L"Close as completed with a comment\x2026");
    AppendMenuW(menu, MF_STRING, 4, L"Close as not planned with a comment\x2026");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1) return;
    bool not_planned = chosen == 2 || chosen == 4;
    char *comment = NULL;
    if (chosen >= 3) {
        char *caption = xstrfmt("Comment on #%d before closing it", s->issue.number);
        comment = dialog_text(app_window(), caption, "Comment", "Next", "");
        free(caption);
        if (!comment) return;
        if (str_empty(comment)) { free(comment); comment = NULL; }
    }
    int open = s->issue.sub_issues - s->issue.sub_issues_done;
    Str why; str_init(&why);
    if (open > 0) str_appendf(&why, "%d of its sub-issues %s still open and stay%s open. ", open, open == 1 ? "is" : "are", open == 1 ? "s" : "");
    if (issue_run_active(s)) str_appendz(&why, "A session is still working on it; closing does not stop it. ");
    if (comment) str_appendz(&why, "Your comment is posted first.");
    char *title = xstrfmt("Close issue #%d as %s?", s->issue.number, not_planned ? "not planned" : "completed");
    bool ok = app_confirm(title, why.len ? why.data : NULL, "Close issue", false);
    free(title); str_free(&why);
    if (!ok || s->closed) { free(comment); return; }
    s->closing = true; set_string(&s->write_error, NULL);
    Json *args = json_object();
    json_set_num(args, "issue", s->issue.number); json_set_str(args, "repo", s->project.repo);
    json_set_str(args, "reason", not_planned ? "not_planned" : "completed");
    if (comment) json_set_str(args, "comment", comment);
    free(comment);
    store_call("close_issue", args, 0, s, issue_close_done, 0, &s->req_close);
    pane_relayout(s->base.pane);
}
static void issue_refresh(Screen *base) {
    IssueScreen *s = (IssueScreen *)base;
    request_cancel(&s->req_runs); request_cancel(&s->req_detail);
    set_string(&s->timeline_seen, NULL);   // the timeline is read again with it
    set_string(&s->edit_error, NULL);
    issue_load(s, true);
    pane_relayout(base->pane); pane_header_changed(base->pane);
}
static void issue_action(Screen *base, int action, intptr_t arg, POINT pt) {
    IssueScreen *s = (IssueScreen *)base;
    switch (action) {
    case ACT_ISSUE_OPEN: open_web_url(s->issue.url); break;
    case ACT_ISSUE_COPY: if (safe_web_url(s->issue.url)) copy_to_clipboard(pane_hwnd(base->pane), s->issue.url); break;
    case ACT_ISSUE_REFRESH: issue_refresh(base); break;
    case ACT_ISSUE_CLOSE: issue_close(s, pt); break;
    case ACT_ISSUE_EDIT: {
        const char *body = json_str(json_get(s->detail, "body"));
        if (s->editing || !body) break;
        char *t = xstrdup(s->issue.title ? s->issue.title : ""), *b = xstrdup(body);
        issue_edit(s, details_edit("issue", s->issue.number, t, b));
        free(t); free(b);
        break;
    }
    case ACT_ISSUE_ASSIGNEES: case ACT_ISSUE_LABELS: {
        if (s->editing) break;
        int number = s->issue.number;
        Json *list = action == ACT_ISSUE_LABELS ? labels_edit(s->issue.labels, s->issue.label_count, number)
                                                : assignees_edit(pane_hwnd(base->pane), pt, s->issue.assignees, s->issue.assignee_count, number);
        if (list) { Json *fields = json_object(); json_object_set(fields, action == ACT_ISSUE_LABELS ? "labels" : "assignees", list); issue_edit(s, fields); }
        pane_relayout(base->pane);
        break;
    }
    case ACT_ISSUE_PARENT: if (s->issue.has_parent) issue_open_link(s, &s->issue.parent); break;
    case ACT_ISSUE_SUB_LINK: if ((size_t)arg < s->sub_count) issue_open_link(s, &s->subs[arg]); break;
    case ACT_ISSUE_EVENT: if ((size_t)arg < json_count(s->events)) event_open(s, json_at(s->events, (size_t)arg)); break;
    case ACT_ISSUE_PROJECT: open_web_url(json_str(json_get(json_at(json_get(s->detail, "projects"), (size_t)arg), "url"))); break;
    case ACT_ISSUE_MORE: if (s->next_page && !s->req_timeline) { issue_timeline_load(s, s->next_page); pane_relayout(base->pane); } break;
    case ACT_ISSUE_SUB: if ((size_t)arg < s->board_issue_count) app_push_detail(issue_detail_screen_new(&s->project, &s->board_issues[arg])); break;
    case ACT_ISSUE_RUN: if ((size_t)arg < s->run_count) app_push_detail(conversation_screen_new(&s->runs[arg])); break;
    case ACT_ISSUE_PULL: {
        if ((size_t)arg >= s->issue.pull_count) break;
        issue_open_pull(s, &s->issue.pulls[arg]);
        break;
    }
    case ACT_ISSUE_CHECKED: s->uncertain = false; set_string(&s->write_error, NULL); pane_relayout(base->pane); break;
    case ACT_ISSUE_START: {
        if (s->busy || s->uncertain) break;
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
static Json *link_json(const BoardLink *l) {
    Json *o = json_object();
    json_set_num(o, "number", l->number); json_set_str(o, "title", l->title); json_set_str(o, "url", l->url); json_set_str(o, "repo", l->repo);
    json_set_bool(o, "draft", l->draft); json_set_str(o, "state", l->state); json_set_str(o, "stateReason", l->state_reason);
    Json *labels = json_array(); for (size_t i = 0; i < l->label_count; i++) { Json *x = json_object(); json_set_str(x, "name", l->labels[i].name); json_set_str(x, "color", l->labels[i].color); json_array_push(labels, x); } json_object_set(o, "labels", labels);
    return o;
}
static void issue_timer(Screen *base, UINT id) {
    IssueScreen *s = (IssueScreen *)base;
    if (poller_fired(&s->poller, id)) { request_cancel(&s->req_runs); issue_load(s, false); }
}
static void issue_visible(Screen *base, bool shown) {
    IssueScreen *s = (IssueScreen *)base;
    if (shown) poller_start(&s->poller, base->pane, TIMER_POLL, 45000);
    else { poller_stop(&s->poller); request_cancel(&s->req_board); request_cancel(&s->req_runs); request_cancel(&s->req_detail); }
}
static void issue_activated(Screen *base, bool active) { if (active) issue_visible(base, true); }
static const ScreenVTable issue_vt = {
    .destroy = issue_destroy, .layout = issue_layout, .header = issue_header, .action = issue_action, .timer = issue_timer,
    .visible = issue_visible, .refresh = issue_refresh, .activated = issue_activated,
};
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
    struct { bool has; time_t when; const char *key; } dates[] = { { issue->has_created, issue->created_at, "createdAt" }, { issue->has_updated, issue->updated_at, "updatedAt" } };
    for (size_t k = 0; k < 2; k++) {
        if (!dates[k].has) continue;
        char iso[40]; struct tm *tm = gmtime(&dates[k].when);
        if (tm) { strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", tm); json_set_str(j, dates[k].key, iso); }
    }
    Json *sub = json_object(); json_set_num(sub, "total", issue->sub_issues); json_set_num(sub, "completed", issue->sub_issues_done); json_object_set(j, "subIssues", sub);
    if (issue->has_parent) json_object_set(j, "parent", link_json(&issue->parent));
    Json *pulls = json_array(); for (size_t i = 0; i < issue->pull_count; i++) json_array_push(pulls, link_json(&issue->pulls[i])); json_object_set(j, "pulls", pulls);
    issue_summary_parse(j, &s->issue);
    json_free(j);
    // The board as last saved fills the sub-issues and pull requests in before the first read answers.
    char *key = xstrfmt("pulls:%s", project->repo);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    if (saved) { issue_board_show(s, saved, true); json_free(saved); }
    // So does the issue's own read, body and timeline, as last seen.
    if (store_supports("issue")) {
        key = issue_cache_key(s, "issue"); saved = cache_value(g_store.cache, key); free(key);
        if (saved) { issue_detail_show(s, json_get(saved, "issue")); json_free(saved); }
    }
    if (store_supports("issue_timeline")) {
        key = issue_cache_key(s, "issue-timeline"); saved = cache_value(g_store.cache, key); free(key);
        if (saved) { issue_timeline_show(s, saved, true); json_free(saved); }
    }
    return &s->base;
}
