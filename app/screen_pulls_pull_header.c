#include "screen_pulls_internal.h"

/// `tabnav`: Sessions, PR Body, Conversation, Files changed, Commits, Checks and Findings with their counts (doc_tab),
/// and the diffstat at the right.
typedef struct { int additions, deletions; } DiffStatData;

static int pull_commit_count(PullScreen *s);
static int diffstat_width(Canvas *cv, const DiffStatData *d);
static void paint_diffstat(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_delete_run_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

int screen_pulls_toolbar_width(Canvas *cv, const ButtonSpec *buttons, size_t count) {
    int w = 0;
    for (size_t i = 0; i < count; i++) w += px(10) * 2 + text_width(cv, buttons[i].text, FONT_FOOTNOTE) + 2 + (i ? px(6) : 0);
    return w;
}

const char *screen_pulls_pull_author(PullScreen *s) {
    const PullSummary *row = screen_pulls_board_row(s);
    const char *author = json_str(json_get(s->pr, "author"));
    if (!author && row) author = row->author;
    return author ? author : s->body_author;
}

static int pull_commit_count(PullScreen *s) { return json_int_or(json_get(s->pr, "commits"), (int)json_count(json_get(s->pr, "commitList"))); }

/// The description as last read, NULL until it is: an edit starts from it.
const char *screen_pulls_pull_body(PullScreen *s) {
    const char *body = json_str(json_get(s->pr, "body"));
    return body ? body : s->body;
}

/// `gh-header`: the title with its muted number and the buttons beside it, then the state and `author wants to merge N commits into base from head`.
void screen_pulls_layout_header(PullScreen *s, Doc *doc, Col c) {
    const PullSummary *row = screen_pulls_board_row(s);
    bool can_merge = store_supports("merge_pull") && str_eq(json_str(json_get(s->pr, "state")), "open") && json_bool_tristate(json_get(s->pr, "draft")) != 1
        && json_str(json_get(s->pr, "headSha")) && json_str(json_get(s->pr, "baseRef"));
    bool can_update_branch = store_supports("update_pull_branch") && str_eq(json_str(json_get(s->pr, "state")), "open")
        && json_str(json_get(s->pr, "headSha")) && json_str(json_get(s->pr, "baseRef"));
    ButtonSpec buttons[5]; size_t bn = 0;
    { ButtonSpec b = { 0, "\xE2\x9F\xB3 Refresh", BUTTON_BORDERED, ACT_RELOAD, 0, !s->req_pull }; buttons[bn++] = b; }
    if (store_supports("update_pull") && screen_pulls_is_open(s)) { ButtonSpec b = { 0, s->editing ? "Saving\xE2\x80\xA6" : "Edit", BUTTON_BORDERED, ACT_EDIT, 0, !s->editing && screen_pulls_pull_body(s) }; buttons[bn++] = b; }
    if (can_update_branch) { ButtonSpec b = { 0, s->updating_branch ? "Updating\xE2\x80\xA6" : "Update branch", BUTTON_BORDERED, ACT_UPDATE_BRANCH, 0, !s->updating_branch && !s->merging }; buttons[bn++] = b; }
    if (can_merge) { ButtonSpec b = { 0, s->merging ? "Merging\xE2\x80\xA6" : "Merge", BUTTON_PROMINENT, ACT_MERGE, 0, !s->merging && !s->busy }; buttons[bn++] = b; }
    if (safe_web_url(json_str(json_get(s->pr, "url")))) { ButtonSpec b = { 0, "Open in GitHub \xE2\x86\x97", BUTTON_BORDERED, ACT_OPEN_URL, 0, true }; buttons[bn++] = b; }
    int tw = screen_pulls_toolbar_width(doc->cv, buttons, bn) + px(2);
    int title_line = font_height(doc->cv, FONT_TITLE) + px(6);
    // Beside the title when there is room, above it when there is not.
    bool beside = c.iw - tw - px(16) >= px(320);
    int top = doc->y, buttons_bottom = top;
    if (beside) { doc->y = top + px(3); doc_button_row(doc, c.ix + c.iw - tw, tw, buttons, bn); buttons_bottom = doc->y; doc->y = top; }
    else { doc_button_row(doc, c.ix, c.iw, buttons, bn); doc_space(doc, px(12)); }
    const char *title = json_str(json_get(s->pr, "title"));
    if (!title && row) title = row->title;
    FlowData *t = screen_pulls_flow_new(title_line);
    char *spaced = xstrfmt("%s ", title ? title : "Pull request"); screen_pulls_flow_add(t, spaced, FONT_TITLE, theme.ink, false); free(spaced);
    char *number = xstrfmt("#%d", s->number); screen_pulls_flow_add(t, number, FONT_TITLE, theme.muted, false); free(number);
    screen_pulls_doc_flow(doc, c.ix, beside ? c.iw - tw - px(16) : c.iw, 0, t);
    if (doc->y < buttons_bottom) doc->y = buttons_bottom;
    doc_space(doc, px(10));
    // The state pill, then the sentence GitHub writes under the title.
    bool draft = json_bool_tristate(json_get(s->pr, "draft")) == 1 || (json_is_null(s->pr) && row && row->draft);
    const char *state = json_str(json_get(s->pr, "state"));
    const char *state_text = draft && screen_pulls_is_open(s) ? "draft" : state ? state : row ? "open" : "loading";
    COLORREF state_color = str_eq(state, "merged") ? theme.accent : str_eq(state, "closed") ? theme.danger : draft ? theme.muted : theme.ok;
    int y = doc->y;
    int pw = screen_pulls_doc_pill(doc, c.ix, y, state_text, state_color);
    if (s->has_stack) { char *label = stack_position_label(&s->stack, s->number); pw += px(8) + screen_pulls_doc_stack_pill(doc, c.ix + pw + px(8), y, label, s->stack_open); free(label); }
    const char *head = json_str(json_get(s->pr, "headRef")), *base_ref = json_str(json_get(s->pr, "baseRef"));
    if (!head && row && *row->branch) head = row->branch;
    if (!base_ref && row && *row->base_branch) base_ref = row->base_branch;
    const char *author = screen_pulls_pull_author(s);
    int commits = pull_commit_count(s);
    FlowData *f = screen_pulls_flow_new(px(26));
    if (author) { char *a = xstrfmt("%s ", author); screen_pulls_flow_add(f, a, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(a); }
    if (head || base_ref || commits > 0) {
        bool done = str_eq(state, "merged");
        Str verb; str_init(&verb);
        str_appendz(&verb, done ? "merged " : author ? "wants to merge " : "Wants to merge ");
        if (done && !author) verb.data[0] = 'M';
        if (commits > 0) str_appendf(&verb, "%d commit%s ", commits, commits == 1 ? "" : "s");
        if (base_ref || head) str_appendz(&verb, "into ");
        screen_pulls_flow_add(f, verb.data, FONT_FOOTNOTE, theme.muted, false);
        str_free(&verb);
        if (base_ref || head) screen_pulls_flow_add(f, base_ref ? base_ref : "main", FONT_MONO_CAPTION2, theme.accent, true);
        if (head) { screen_pulls_flow_add(f, "from ", FONT_FOOTNOTE, theme.muted, false); screen_pulls_flow_add(f, head, FONT_MONO_CAPTION2, theme.accent, true); }
    }
    if (row && str_eq(row->mergeable, "conflicting")) { char *w = xstrfmt("\xC2\xB7 \xE2\x9A\xA0 conflicts with %s ", base_ref ? base_ref : "its base"); screen_pulls_flow_add(f, w, FONT_FOOTNOTE, theme.warning, false); free(w); }
    if (row && row->has_updated) { char *rel = format_relative(row->updated_at); char *u = xstrfmt("\xC2\xB7 updated %s", rel); screen_pulls_flow_add(f, u, FONT_FOOTNOTE, theme.muted, false); free(u); free(rel); }
    if (f->count) screen_pulls_doc_flow(doc, c.ix + pw + px(10), c.iw - pw - px(10), 0, f); else screen_pulls_flow_free(f);
    if (doc->y < y + px(26)) doc->y = y + px(26);
    if (s->has_stack && s->stack_open) { doc_space(doc, px(10)); screen_pulls_layout_stack_overview(s, doc, c); }
    if (s->merge_error) { doc_space(doc, px(8)); doc_notice(doc, c.ix, c.iw, s->merge_error); }
    if (s->edit_error) { doc_space(doc, px(8)); doc_notice(doc, c.ix, c.iw, s->edit_error); }
    if (s->branch_note) { doc_space(doc, px(8)); doc_label(doc, c.ix, c.iw, 0xE895, s->branch_note, FONT_FOOTNOTE, theme.secondary); }
}

/// Whether every list the server offers has been read once.
bool screen_pulls_conv_read(PullScreen *s) {
    for (int k = 0; k < CONV_FEEDS; k++) if (store_supports(screen_pulls_conv_feeds[k].op) && !s->conv[k].read) return false;
    return true;
}

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

/// The trash button at the left end of the open Run tab: the button, with the tab's underline running on beneath it.
static void paint_delete_run_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT u = { rc->left + px(4), rc->bottom - px(2), rc->right + px(12), rc->bottom };
    fill_round_rect(cv, &u, px(1), theme.accent, theme.accent);
    int bh = px(26), top = rc->top + (rc->bottom - px(2) - rc->top - bh) / 2;
    RECT b = { rc->left + px(4), top, rc->left + px(4) + px(28), top + bh };
    screen_pulls_paint_delete_run(doc, it, cv, &b);
}

/// The session the Run tab shows: the one serving it, else the one being prepared for it.
const char *screen_pulls_run_target(PullScreen *s) { return s->run_session ? s->run_session : s->log_session; }

void screen_pulls_layout_tabs(PullScreen *s, Doc *doc, Col c) {
    int h = px(42), x = c.ix, y = doc->y, right = c.ix + c.iw;
    bool loaded = !json_is_null(s->pr);
    double additions = 0, deletions = 0;
    bool has_diff = json_num(json_get(s->pr, "additions"), &additions) && json_num(json_get(s->pr, "deletions"), &deletions);
    DiffStatData *ds = NULL; int dsw = 0;
    if (has_diff) { ds = xcalloc(1, sizeof *ds); ds->additions = (int)additions; ds->deletions = (int)deletions; dsw = diffstat_width(doc->cv, ds); right -= dsw + px(16); }
    char count[24];
    const PullSummary *row = screen_pulls_board_row(s);
    if (s->run_count) { snprintf(count, sizeof count, "%zu", s->run_count); doc_tab(doc, &x, &y, c.ix, right, h, 0xE8F2, "Sessions", count, s->tab == PR_TAB_SESSIONS, ACT_PR_TAB, PR_TAB_SESSIONS); }
    doc_tab(doc, &x, &y, c.ix, right, h, 0xE7C3, "PR Body", NULL, s->tab == PR_TAB_BODY, ACT_PR_TAB, PR_TAB_BODY);
    if (store_supports(screen_pulls_conv_feeds[CONV_COMMENTS].op)) {
        // The messages once they are read; until then the board's count of conversation comments.
        double comments = 0;
        bool has_count = screen_pulls_conv_read(s) ? (comments = (double)pull_conversation_count(s->conv[CONV_COMMENTS].items, s->conv[CONV_REVIEWS].items, s->conv[CONV_REVIEW_COMMENTS].items), true) : row && json_num(json_get(row->raw, "comments"), &comments);
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
        char *title = s->profile_count ? xstrfmt("Run \xC2\xB7 %s \xE2\x96\xBE", screen_pulls_run_shown_profile(s)) : xstrdup("Run");
        doc_tab(doc, &x, &y, c.ix, right, h, 0xE768, title, NULL, on, ACT_PR_TAB, PR_TAB_RUN);
        free(title);
        Item *tab = doc_item(doc, (int)doc->count - 1);
        if (on && s->profile_count) { tab->action = ACT_RUN_PROFILE; tab->hand = true; }
        // Inside the open tab, at its left end, a trash button deletes the run's session, and the workspace serving it
        // with it. The tab moves right to make room, and the button carries the tab's underline on under itself.
        if (on && screen_pulls_run_target(s) && store_supports("delete")) {
            int slot = px(28);
            RECT br = { tab->rc.left, tab->rc.top, tab->rc.left + slot, tab->rc.bottom };
            OffsetRect(&tab->rc, slot, 0); x += slot;
            Item *it = doc_item(doc, doc_add(doc, &br, paint_delete_run_tab));
            it->data = str_eq(s->deleting_run, screen_pulls_run_target(s)) ? s : NULL;   // borrowed owner marks the pending deletion
            if (!s->deleting_run) { it->action = ACT_DELETE_SERVED; it->hand = true; }
        }
    }
    if (ds) { doc->y = y; doc_custom(doc, c.ix + c.iw - dsw, dsw, h, paint_diffstat, ds, free, 0, 0); }
    doc->y = y + h;
    doc_rule(doc, c.x, c.w);
}
