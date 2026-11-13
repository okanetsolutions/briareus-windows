// The board's Issues tab: the open issues the filters keep, sub-issues under their epic, and, when the board listed only
// its first 100, Load more for the next page from `issues` (a failed one read again), with why the last read failed.
// core/issue_pages.c decides where each page starts and which rows it adds.
#include "screen_pulls_internal.h"

static void issue_pages_done(void *owner, Request *req);

IssueSummary *screen_pulls_listed_issues(PullsScreen *s, size_t *count) {
    return issue_pages_listed(&s->issue_pages, s->board, s->issues, s->issue_count, count);
}

static void issue_pages_done(void *owner, Request *req) {
    PullsScreen *s = owner;
    if (req->client != g_store.client || !store_supports(req->operation)) { pane_relayout(s->base.pane); return; }
    if (!req->ok || !issue_pages_receive(&s->issue_pages, req->result)) {
        char *why = request_error_or_unexpected(req); issue_pages_fail(&s->issue_pages, why); free(why);
    }
    pane_relayout(s->base.pane);
}

/// The next page, or the one that failed again. Reads only; trying again is always safe.
void screen_pulls_issue_pages_load(PullsScreen *s) {
    if (s->req_issue_pages || !issue_pages_can_load_more(&s->issue_pages, s->board, store_supports("issues"))) return;
    Json *args = issue_pages_query(&s->issue_pages, s->project.repo, s->board);
    issue_pages_reading(&s->issue_pages);
    store_call("issues", args, 0, s, issue_pages_done, 0, &s->req_issue_pages);
    pane_relayout(s->base.pane);
}

/// A refresh reads the board afresh; its pages start again after it.
void screen_pulls_issue_pages_reset(PullsScreen *s) {
    request_cancel(&s->req_issue_pages);
    issue_pages_clear(&s->issue_pages);
}

/// An issue this app closed leaves the loaded pages of its board, the root of the pane it was closed in, which the
/// board's next read would not drop.
void screen_pulls_board_issue_closed(Pane *pane, const char *repo, int number) {
    Screen *root = pane ? pane_root(pane) : NULL;
    if (!root || root->vt->layout != screen_pulls_pulls_layout) return;
    PullsScreen *s = (PullsScreen *)root;
    if (str_eq(s->project.repo, repo)) issue_pages_drop(&s->issue_pages, number);
}

static void layout_more(PullsScreen *s, Doc *doc, int w) {
    bool routed = store_supports("issues");
    const char *error = issue_pages_retry_error(&s->issue_pages, s->board, routed);
    if (error) { doc_notice(doc, 0, w, error); doc_space(doc, px(6)); }
    if (issue_pages_can_load_more(&s->issue_pages, s->board, routed)) {
        const char *label = s->req_issue_pages ? "Loading\xE2\x80\xA6" : issue_pages_retries(&s->issue_pages) ? "Retry" : "Load more";
        doc_button(doc, 0, 0, label, BUTTON_BORDERED, ACT_MORE_ISSUES, 0, !s->req_issue_pages);
        doc_space(doc, px(8));
    }
    if (issue_pages_cut_short(&s->issue_pages, s->board, routed)) doc_text(doc, 0, w, "This repository has more open issues; only the most recently updated are listed.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
}

/// `rows` are the board rows of the listed issues, `shown` how many of them the filters keep.
void screen_pulls_issues_layout(PullsScreen *s, Doc *doc, int w, const BoardRow *rows, size_t shown) {
    const char *refused = json_str(json_get(s->board, "issuesError"));
    if (refused) {
        int box = doc_box_begin(doc, 0, w, px(10), theme.raise, theme.line, px(12));
        doc_item(doc, box)->hover_fill = false;
        doc_text(doc, px(12), w - px(24), "GitHub would not read this repository\xE2\x80\x99s issues with the server\xE2\x80\x99s token. A fine-grained token needs Issues: read. The pull requests are unaffected.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(6));
        doc_notice(doc, px(12), w - px(24), refused);
        doc_box_end(doc, box, px(10));
        return;
    }
    const BoardFilter *filter = screen_pulls_current_filter(s);
    size_t count; IssueSummary *listed = screen_pulls_listed_issues(s, &count);
    IssueSummary *visible = xcalloc(count + 1, sizeof *visible);
    size_t *map = xcalloc(count + 1, sizeof *map);
    size_t vn = 0;
    for (size_t i = 0; i < count; i++) if (board_filter_passes(filter, &rows[i], -1)) { visible[vn] = listed[i]; map[vn] = i; vn++; }
    size_t nn; IssueRow *nested = issues_nested(visible, vn, s->project.repo, &nn);
    nn = issues_folded(nested, nn, visible, &s->issue_folds);
    for (size_t k = 0; k < nn; k++) {
        int depth = nested[k].depth > 4 ? 4 : nested[k].depth;
        int indent = depth * px(18);
        doc_issue_row_foldable(doc, indent, w - indent, &listed[map[nested[k].index]], s->project.repo, depth > 0, ACT_OPEN_ISSUE, (intptr_t)map[nested[k].index],
            nested[k].children ? ACT_FOLD_ISSUE : 0, issue_folds_contains(&s->issue_folds, visible[nested[k].index].number));
        doc_space(doc, px(8));
    }
    free(nested); free(map); free(visible); free(listed);
    layout_more(s, doc, w);
    if (s->loaded && !shown && !s->error) doc_text(doc, 0, w, count ? "No issues match the filters." : "No open issues.", FONT_FOOTNOTE, theme.muted, DT_LEFT);
}
