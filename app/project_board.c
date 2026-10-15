#include "project_board_internal.h"

static char *cache_key(const BoardTab *p);
static char *filter_key(const BoardTab *p);
static void filter_save(const BoardTab *p);
static void filter_restore(BoardTab *p);
static void settle(BoardTab *p, ProjectBoard *board);
static void show(BoardTab *p, const Json *answer);
static void board_done(void *owner, Request *req);
static void open_card(BoardTab *p, const ProjectCard *card);
static void open_card_pull(BoardTab *p, const ProjectCard *card, size_t index);
static void set_assignee(BoardTab *p, const char *assignee);
static void pick_assignee(BoardTab *p, POINT pt);

// The board's cards are read through its view's filter, a GitHub search that lags behind a move by seconds: a read
// just after one shows the card where it was, and the server keeps that read for 45 seconds. The cards moved lately
// outlive their tab, so a project closed and opened again still shows them where they went.
Settling *project_board_settling;

size_t project_board_settling_count;

bool board_tab_offered(const Project *project) { return project->has_board && store_supports("project_board"); }

void project_board_relayout(BoardTab *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

static char *cache_key(const BoardTab *p) { return xstrfmt("project-board:%s", p->project.repo); }

// The pick is kept on disk per repository, as the other tabs' pickers are.
static char *filter_key(const BoardTab *p) { return xstrfmt("project-board-filter:%s", p->project.repo); }

static void filter_save(const BoardTab *p) {
    Json *saved = json_object();
    if (!str_empty(p->assignee)) json_set_str(saved, "assignee", p->assignee);
    char *key = filter_key(p); cache_store(g_store.cache, saved, key); free(key);
    json_free(saved);
}

static void filter_restore(BoardTab *p) {
    char *key = filter_key(p);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    set_string(&p->assignee, json_str_nonempty(json_get(saved, "assignee")));
    json_free(saved);
}

void project_board_settled(size_t i) {
    Settling *m = &project_board_settling[i];
    for (size_t k = 0; k < m->from_count; k++) free(m->from[k]);
    free(m->repo); free(m->id); free(m->from); free(m->column);
    project_board_settling[i] = project_board_settling[--project_board_settling_count];
}

/// Puts the cards GitHub moved lately in their new columns, on a read that has them in their old ones.
static void settle(BoardTab *p, ProjectBoard *board) {
    time_t now = time(NULL);
    for (size_t i = 0; i < project_board_settling_count;) {
        const Settling *m = &project_board_settling[i];
        if (now > m->until) { project_board_settled(i); continue; }
        if (!str_eq(m->repo, p->project.repo)) { i++; continue; }
        size_t column, card;
        bool found = project_board_find(board, m->id, &column, &card);
        int to = project_board_column(board, m->column);
        bool stale = false;
        for (size_t k = 0; found && !stale && k < m->from_count; k++) stale = str_eq(board->columns[column].id, m->from[k]);
        // Once a read shows it there or anywhere but where it came from (moved again since), the read is GitHub's own.
        if (found && ((int)column == to || !stale)) { project_board_settled(i); continue; }
        if (found && to >= 0) project_board_move(board, column, card, (size_t)to);
        i++;
    }
}

static void show(BoardTab *p, const Json *answer) {
    ProjectBoard board;
    if (!project_board_parse(answer, &board)) return;
    project_board_keep_repo(&board, p->project.repo);
    settle(p, &board);
    project_board_free(&p->board);
    p->board = board; p->has_board = true;
}

static void board_done(void *owner, Request *req) {
    BoardTab *p = owner;
    p->loaded = true;
    if (!req->ok) {
        // A move GitHub refused and no board to show instead: the card goes back by hand.
        if (!p->move_req) project_board_undo_move(p);
        request_error_into(&p->error, req); project_board_relayout(p); return;
    }
    if (!p->move_req) set_string(&p->moved_id, NULL);
    set_string(&p->error, NULL);
    // A carried card's arg is its place on the board shown: dropped after the new one replaces it, it would name another card.
    if (p->host->pane && pane_top(p->host->pane) == p->host) pane_cancel_carry(p->host->pane, p->base + A_CARD);
    show(p, req->result);
    // A refusal is not saved: the board last read stays the one shown the next time.
    if (p->has_board && !p->board.error) { char *key = cache_key(p); cache_store(g_store.cache, req->result, key); free(key); }
    project_board_relayout(p);
}

void project_board_load(BoardTab *p, bool fresh) {
    if (!board_tab_offered(&p->project)) return;
    if (!p->has_board) {
        char *key = cache_key(p);
        Json *saved = cache_value(g_store.cache, key);
        free(key);
        if (saved) { show(p, saved); json_free(saved); }
    }
    request_cancel(&p->req);
    Json *args = json_object(); json_set_str(args, "repo", p->project.repo);
    if (fresh) json_set_str(args, "fresh", "1");
    store_call("project_board", args, 0, p, board_done, 0, &p->req);
}

void board_tab_set_pulls(BoardTab *p, const IssueSummary *issues, size_t issue_count, const PullSummary *pulls, size_t pull_count) {
    p->issues = issues; p->issue_count = issue_count; p->pulls = pulls; p->pull_count = pull_count;
}

// While a card is moving, the read after the move is the first.
void board_tab_open(BoardTab *p) { if (!p->loaded && !p->req && !p->move_req) project_board_load(p, false); }

void board_tab_refresh(BoardTab *p) { set_string(&p->move_error, NULL); project_board_load(p, true); project_board_relayout(p); }

// MARK: - Actions

static void open_card(BoardTab *p, const ProjectCard *card) {
    bool here = card->repo && str_ieq(card->repo, p->project.repo);
    if (here && str_eq(card->type, "issue") && store_supports("issue")) {
        // The issue screen reads the rest itself; the card gives it what to show until it has.
        IssueSummary issue = { 0 };
        issue.number = card->number; issue.title = card->title; issue.url = card->url; issue.author = card->author;
        issue.assignees = card->assignees; issue.assignee_count = card->assignee_count;
        issue.labels = card->labels; issue.label_count = card->label_count;
        issue.has_created = card->has_created; issue.created_at = card->created_at;
        if (card->has_parent) { issue.has_parent = true; issue.parent = card->parent; }
        app_set_overlay(issue_detail_screen_new(&p->project, &issue));
        return;
    }
    if (here && str_eq(card->type, "pull") && store_supports("pull")) { app_set_overlay(pull_detail_screen_new(&p->project, card->number, NULL, NULL)); return; }
    if (safe_web_url(card->url)) open_web_url(card->url);
}

/// A pull request of this project opens in the side panel, as its card would; any other on GitHub.
static void open_card_pull(BoardTab *p, const ProjectCard *card, size_t index) {
    size_t n; BoardLink *pulls = project_board_card_pulls(p, card, &n);
    if (index < n) {
        const BoardLink *l = &pulls[index];
        if (!board_link_is_foreign(l, p->project.repo) && store_supports("pull"))
            app_set_overlay(pull_detail_screen_new(&p->project, l->number, NULL, pulls_find(p->pulls, p->pull_count, l->number)));
        else if (safe_web_url(l->url)) open_web_url(l->url);
    }
    project_board_card_pulls_free(pulls, n);
}

static void set_assignee(BoardTab *p, const char *assignee) {
    set_string(&p->assignee, str_empty(assignee) ? NULL : assignee);
    filter_save(p);
    project_board_relayout(p);
}

/// The picker's menu: everyone, then each assignee with how many cards they have.
static void pick_assignee(BoardTab *p, POINT pt) {
    size_t count; FilterOption *options = project_board_assignees(&p->board, p->assignee, &count);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (str_empty(p->assignee) ? MF_CHECKED : 0), 1, L"All assignees");
    for (size_t i = 0; i < count; i++) {
        char *label = xstrfmt("%s (%d)", options[i].text, options[i].count); wchar_t *wl = utf8_to_wide(label);
        bool picked = !str_empty(p->assignee) && str_ieq(p->assignee, options[i].value);
        AppendMenuW(menu, MF_STRING | (picked ? MF_CHECKED : 0), (UINT_PTR)(2 + i), wl);
        free(label); free(wl);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) set_assignee(p, NULL);
    else if (chosen >= 2 && (size_t)(chosen - 2) < count) set_assignee(p, options[chosen - 2].value);
    filter_options_free(options, count);
}

bool board_tab_action(BoardTab *p, int action, intptr_t arg, POINT pt) {
    if (action < p->base || action >= p->base + BOARD_TAB_ACTIONS) return false;
    switch (action - p->base) {
    case A_CARD: { const ProjectCard *card = project_board_card_at(p, arg); if (card) open_card(p, card); break; }
    case A_CARD_PULL: { const ProjectCard *card = project_board_card_at(p, arg >> 8); if (card) open_card_pull(p, card, (size_t)(arg & 0xFF)); break; }
    case A_FILTER_ASSIGNEE: if (p->host->pane) pick_assignee(p, pt); break;
    case A_CLEAR_FILTER: set_assignee(p, NULL); break;
    case A_OPEN_GITHUB: { const char *url = safe_web_url(p->board.view_url) ? p->board.view_url : p->board.url; if (safe_web_url(url)) open_web_url(url); break; }
    }
    return true;
}

// MARK: - Lifetime

BoardTab *board_tab_new(const Project *project, Screen *host, int action_base) {
    BoardTab *p = xcalloc(1, sizeof *p);
    project_copy(&p->project, project);
    p->host = host; p->base = action_base; p->drop_column = -1;
    filter_restore(p);
    return p;
}

void board_tab_free(BoardTab *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->move_req);
    // The side panel showed one of this board's cards. A board that was never
    // shown (a duplicate `app_show_detail` throws away) owns no panel.
    if (p->host->pane) app_set_overlay(NULL);
    project_board_free(&p->board);
    project_free(&p->project); free(p->error); free(p->move_error); free(p->moved_id); free(p->moved_off); free(p->moved_to); free(p->assignee);
    free(p);
}
