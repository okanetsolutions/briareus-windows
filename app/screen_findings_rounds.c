#include "screen_findings_internal.h"

// what a send came back with, once its card is gone
typedef struct { size_t *rounds; size_t count; const char *repo; int pr; } Group;

static size_t build_groups(FindingsScreen *s, Group **out);
static void groups_free(Group *groups, size_t n);
static void clickable(Doc *doc, int item, int action, intptr_t arg);
static void layout_outcome(FindingsScreen *s, Doc *doc, int x, int w, size_t i);
static void layout_finding(FindingsScreen *s, Doc *doc, int fx, int fw, size_t r, size_t k, bool mine, bool busy);
static void layout_round(FindingsScreen *s, Doc *doc, int x, int w, size_t r);
static void layout_group(FindingsScreen *s, Doc *doc, int x, int w, const Group *g);

/// Rounds by the pull request they were left on, in the queue's oldest-first order.
static size_t build_groups(FindingsScreen *s, Group **out) {
    Group *groups = xcalloc(s->round_count, sizeof *groups);
    size_t n = 0;
    for (size_t i = 0; i < s->round_count; i++) {
        const char *repo = session_repo(screen_findings_round_session(s, i));
        int pr = held_round_pr_number(s->rounds[i].held);
        size_t g = 0;
        while (g < n && !(groups[g].pr == pr && str_ieq(groups[g].repo, repo))) g++;
        if (g == n) { groups[n].repo = repo ? repo : ""; groups[n].pr = pr; groups[n].rounds = xcalloc(s->round_count, sizeof *groups[n].rounds); n++; }
        groups[g].rounds[groups[g].count++] = i;
    }
    *out = groups;
    return n;
}

static void groups_free(Group *groups, size_t n) { for (size_t i = 0; i < n; i++) free(groups[i].rounds); free(groups); }

// MARK: - Layout

static void clickable(Doc *doc, int item, int action, intptr_t arg) { doc_item(doc, item)->action = action; doc_item(doc, item)->arg = arg; doc_item(doc, item)->hand = true; }

static void layout_outcome(FindingsScreen *s, Doc *doc, int x, int w, size_t i) {
    const Outcome *o = &s->outcomes[i];
    int box = doc_box_begin(doc, x, w, px(8), theme.elevated, theme.border, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(10), iw = w - px(20), close_w = px(22);
    int tw = text_width(doc->cv, o->title, FONT_CAPTION_SEMIBOLD) + px(2);
    if (tw > iw / 2) tw = iw / 2;
    int top = doc->y, lh = font_height(doc->cv, FONT_CAPTION) + px(2);
    RECT tr = { ix, top, ix + tw, top + lh };
    int ti = doc_text_at(doc, &tr, o->title, FONT_CAPTION_SEMIBOLD, theme.text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    clickable(doc, ti, ACT_OUTCOME_SESSION, (intptr_t)i);
    int mx = ix + tw + px(8), mw = iw - tw - px(8) - close_w;
    doc->y = top;
    doc_text(doc, mx, mw, o->text, FONT_CAPTION, o->danger ? theme.danger : theme.secondary, DT_WORDBREAK);
    if (doc->y < top + lh) doc->y = top + lh;
    RECT cr = { ix + iw - close_w + px(4), top, ix + iw, top + lh };
    wchar_t glyph[2] = { 0xE711, 0 }; char *close = wide_to_utf8(glyph);
    int ci = doc_text_at(doc, &cr, close, FONT_ICON_SMALL, theme.secondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    free(close);
    clickable(doc, ci, ACT_DISMISS_OUTCOME, (intptr_t)i);
    doc_box_end(doc, box, px(8));
    doc_space(doc, px(8));
}

static void layout_finding(FindingsScreen *s, Doc *doc, int fx, int fw, size_t r, size_t k, bool mine, bool busy) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    const Json *f = json_at(json_get(held, "findings"), k);
    const char *sid = session_id(ses), *key = json_str(json_get(f, "key"));
    int round = screen_findings_round_number(held);
    intptr_t arg = (intptr_t)(r * FINDING_STRIDE + k);
    doc_rule(doc, fx, fw);
    doc_space(doc, px(8));
    COLORREF sev_color;
    BadgeSpec sev = { 0, finding_severity_label(json_str(json_get(f, "severity")), &sev_color), 0, false };
    sev.color = sev_color;
    int chip_w = doc_badges_width(doc, &sev, 1);
    int top = doc->y;
    doc_badges(doc, fx, chip_w, &sev, 1, theme.elevated);
    int chip_h = doc->y - top;
    doc->y = top;
    const char *ft = json_str(json_get(f, "title"));
    int ti = doc_text(doc, fx + chip_w + px(6), fw - chip_w - px(6), ft ? ft : "Finding", FONT_FOOTNOTE, theme.ink, DT_WORDBREAK);
    clickable(doc, ti, ACT_OPEN_FINDING, arg);
    if (doc->y < top + chip_h) doc->y = top + chip_h;
    const char *file = json_str_nonempty(json_get(f, "file"));
    if (file) {
        int line = json_int_or(json_get(f, "line"), 0);
        char *loc = line ? xstrfmt("%s:%d \xE2\x86\x97", file, line) : xstrfmt("%s \xE2\x86\x97", file);
        doc_space(doc, px(4));
        int li = doc_text(doc, fx, fw, loc, FONT_MONO_CAPTION2, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        clickable(doc, li, ACT_OPEN_FINDING, arg);
        free(loc);
    }
    if (json_is_set(json_get(f, "parked"))) {
        const char *why = json_str_nonempty(json_get(f, "parkedWhy")), *parked = json_str_nonempty(json_get(f, "parked"));
        char *advice = xstrfmt("The loop would have parked it: %s.", why ? why : parked ? parked : "");
        doc_space(doc, px(3));
        doc_text(doc, fx, fw, advice, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        free(advice);
    }
    if (!store_can_manage() || !key) return;
    if (mine) {
        if (!store_supports("complete_findings")) return;
        const char *decision = screen_findings_decision_of(s, sid, round, f);
        int selected = finding_decision_index(decision);
        doc_space(doc, px(4));
        doc_segments(doc, fx, fw, finding_decision_titles, FINDING_DECISION_COUNT, selected, ACT_VERDICT, arg * 4, !busy);
        doc_space(doc, px(4));
        const char *reason = screen_findings_reason_of(s, sid, round, f);
        const char *placeholder = "Comment (saved to the pull request)";
        int cb = doc_box_begin(doc, fx, fw, px(3), theme.field, theme.line, px(4));
        doc_item(doc, cb)->hover_fill = false;
        doc_text(doc, fx + px(6), fw - px(12), *reason ? reason : placeholder, FONT_CAPTION2, *reason ? theme.ink : theme.muted, DT_WORDBREAK);
        doc_box_end(doc, cb, px(3));
        if (!busy) doc_box_action(doc, cb, ACT_COMMENT, arg);
    } else {
        bool replying = screen_findings_writing(s, WRITE_REPLY, sid, key), deleting = screen_findings_writing(s, WRITE_DELETE, sid, key);
        ButtonSpec buttons[2]; size_t n = 0;
        if (store_supports("reply_finding")) { ButtonSpec b = { 0xE97A, replying ? "Replying\xE2\x80\xA6" : "Reply", BUTTON_BORDERED, ACT_REPLY, arg, !busy }; buttons[n++] = b; }
        if (store_supports("delete_finding")) { ButtonSpec b = { 0xE74D, deleting ? "Deleting\xE2\x80\xA6" : "Delete from the review", BUTTON_DESTRUCTIVE, ACT_DELETE, arg, !busy }; buttons[n++] = b; }
        if (n) { doc_space(doc, px(6)); doc_button_row(doc, fx, fw, buttons, n); }
        const Replied *rp = screen_findings_replied_find(s, sid, key);
        if (rp && rp->error) { doc_space(doc, px(4)); char *m = xstrfmt("Not replied: %s", rp->error); doc_text(doc, fx, fw, m, FONT_CAPTION, theme.danger, DT_WORDBREAK); free(m); }
        else if (rp) {
            doc_space(doc, px(4));
            int li = doc_text(doc, fx, fw, rp->url ? "Replied \xC2\xB7 on the pull request \xE2\x86\x97" : "Replied", FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
            if (rp->url) clickable(doc, li, ACT_REPLIED_URL, arg);
        }
    }
}

static void layout_round(FindingsScreen *s, Doc *doc, int x, int w, size_t r) {
    const Session *ses = screen_findings_round_session(s, r);
    const Json *held = s->rounds[r].held;
    const char *sid = session_id(ses);
    int round = screen_findings_round_number(held);
    bool mine = held_round_is_mine(held), standalone = json_is_set(json_get(held, "standalone"));
    bool manage = store_can_manage(), busy = s->req_write != NULL;
    bool sending = screen_findings_writing(s, WRITE_COMPLETE, sid, NULL), saving = screen_findings_writing(s, WRITE_SAVE, sid, NULL);
    const Json *findings = json_get(held, "findings");
    size_t n = json_count(findings);
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
    doc_item(doc, box)->hover_fill = false;
    int ix = x + px(12), iw = w - px(24);
    // The conversation's title, and what round this is and since when.
    Str meta; str_init(&meta);
    if (standalone) str_appendz(&meta, "code review"); else str_appendf(&meta, "round %d", round);
    time_t held_at;
    if (board_date_parse(json_str(json_get(held, "heldAt")), &held_at)) { char *when = format_event_time(held_at); str_appendf(&meta, " \xC2\xB7 held since %s", when); free(when); }
    const char *title = session_display_title(ses);
    int top = doc->y, th = px(22);
    int tw = text_width(doc->cv, title, FONT_BODY_SEMIBOLD) + px(4), mw = text_width(doc->cv, meta.data, FONT_CAPTION) + px(4);
    if (tw + px(8) + mw <= iw) {
        RECT tr = { ix, top, ix + tw, top + th };
        int ti = doc_text_at(doc, &tr, title, FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        clickable(doc, ti, ACT_OPEN_SESSION, (intptr_t)r);
        RECT mr = { ix + tw + px(8), top, ix + iw, top + th };
        doc_text_at(doc, &mr, meta.data, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        doc->y = top + th;
    } else {
        int ti = doc_text(doc, ix, iw, title, FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK);
        clickable(doc, ti, ACT_OPEN_SESSION, (intptr_t)r);
        doc_space(doc, px(2));
        doc_text(doc, ix, iw, meta.data, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    str_free(&meta);
    if (json_is_set(json_get(held, "stale"))) {
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, "The branch moved after this round was reviewed: some of these may already be fixed. Sending with nothing to fix reviews the new commits instead of closing the loop.", FONT_CAPTION, theme.danger, DT_WORDBREAK);
    }
    // How this card is worked: findings on the user's own pull request are triaged here, because the verdicts are what
    // the fix session works from; a review of somebody else's is only read, its author answers what it found.
    char *count = xstrfmt("%zu finding%s", n, n == 1 ? "" : "s");
    char *how;
    if (!mine) how = n ? xstrfmt("%s, already on the pull request. Read them there; Reply says something on a finding\xE2\x80\x99s own thread, Delete takes one out of the review itself, and Complete takes this card off the queue and leaves the rest to the pull request\xE2\x80\x99s author.", count)
                      : xstrdup("Every finding was deleted from the review. Complete takes this card off the queue.");
    else if (!manage) how = xstrfmt("%s. This device is read-only: the verdicts are given on the dashboard.", count);
    else how = xstrfmt("%s. Give each one a verdict and a comment if you have one; Save comments keeps them here and on the pull request, and Complete appears once every finding is marked.", count);
    doc_space(doc, px(4));
    doc_text(doc, ix, iw, how, FONT_CAPTION, theme.muted, DT_WORDBREAK);
    free(how); free(count);
    if (mine && manage && n && store_supports("complete_findings")) {
        ButtonSpec all[2] = { { 0, "Fix all", BUTTON_PLAIN, ACT_FIX_ALL, (intptr_t)r, !busy }, { 0, "Clear", BUTTON_PLAIN, ACT_CLEAR_ALL, (intptr_t)r, !busy } };
        doc_space(doc, px(4));
        doc_button_row(doc, ix, iw, all, 2);
    }
    doc_space(doc, px(8));
    for (size_t k = 0; k < n; k++) { layout_finding(s, doc, ix, iw, r, k, mine, busy); doc_space(doc, px(8)); }
    const Card *card = screen_findings_card_find(s, sid);
    if (manage && store_supports("complete_findings")) {
        doc_rule(doc, ix, iw);
        doc_space(doc, px(8));
        if (mine) {
            const char *note = screen_findings_note_of(s, sid, round);
            int nb = doc_box_begin(doc, ix, iw, px(3), theme.field, theme.line, px(4));
            doc_item(doc, nb)->hover_fill = false;
            doc_text(doc, ix + px(6), iw - px(12), *note ? note : "A note for the pull request and the fix session (optional)", FONT_CAPTION2, *note ? theme.ink : theme.muted, DT_WORDBREAK);
            doc_box_end(doc, nb, px(3));
            if (!busy) doc_box_action(doc, nb, ACT_NOTE, (intptr_t)r);
            doc_space(doc, px(8));
            size_t fixes = 0, unruled = 0;
            for (size_t k = 0; k < n; k++) {
                const char *d = screen_findings_decision_of(s, sid, round, json_at(findings, k));
                if (str_eq(d, "fix")) fixes++;
                if (!*d) unruled++;
            }
            ButtonSpec buttons[2]; size_t bn = 0;
            if (store_supports("save_findings")) { ButtonSpec b = { 0, saving ? "Saving\xE2\x80\xA6" : "Save comments", BUTTON_BORDERED, ACT_SAVE, (intptr_t)r, !busy }; buttons[bn++] = b; }
            char *complete = sending ? xstrdup("Completing\xE2\x80\xA6") : fixes ? xstrfmt("Complete \xC2\xB7 send %zu to be fixed", fixes) : xstrdup("Complete \xC2\xB7 nothing to fix, approve and close");
            if (!unruled) { ButtonSpec b = { 0, complete, BUTTON_PROMINENT, ACT_COMPLETE, (intptr_t)r, !busy }; buttons[bn++] = b; }
            if (bn) doc_button_row(doc, ix, iw, buttons, bn);
            free(complete);
            if (unruled) {
                char *m = xstrfmt("%zu finding%s still unmarked; Complete appears once every finding has a verdict.", unruled, unruled == 1 ? "" : "s");
                doc_space(doc, px(6));
                doc_text(doc, ix, iw, m, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
                free(m);
            }
            if (card && card->saved) {
                doc_space(doc, px(6));
                int li = doc_text(doc, ix, iw, card->saved_url ? "Saved \xC2\xB7 comment on the pull request \xE2\x86\x97" : card->saved, FONT_CAPTION, card->saved_failed ? theme.danger : theme.secondary, DT_WORDBREAK);
                if (card->saved_url) clickable(doc, li, ACT_SAVED_URL, (intptr_t)r);
            } else {
                const char *saved_at = json_str_nonempty(json_get(json_get(held, "drafts"), "savedAt"));
                time_t at;
                if (saved_at && board_date_parse(saved_at, &at)) {
                    char *when = format_event_time(at), *m = xstrfmt("Comments saved %s", when);
                    doc_space(doc, px(6));
                    doc_text(doc, ix, iw, m, FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
                    free(when); free(m);
                }
            }
        } else {
            ButtonSpec b = { 0, sending ? "Completing\xE2\x80\xA6" : "Complete", BUTTON_PROMINENT, ACT_COMPLETE, (intptr_t)r, !busy };
            doc_button_row(doc, ix, iw, &b, 1);
        }
    }
    if (card && card->info) { doc_space(doc, px(6)); doc_text(doc, ix, iw, card->info, FONT_CAPTION, card->info_danger ? theme.danger : theme.secondary, DT_WORDBREAK); }
    if (card && card->error) { doc_space(doc, px(6)); doc_notice(doc, ix, iw, card->error); }
    doc_box_end(doc, box, px(10));
    doc_space(doc, px(12));
}

static void layout_group(FindingsScreen *s, Doc *doc, int x, int w, const Group *g) {
    size_t findings = 0;
    for (size_t i = 0; i < g->count; i++) findings += json_count(json_get(s->rounds[g->rounds[i]].held, "findings"));
    char *heading = xstrfmt("%s \xC2\xB7 PR #%d \xE2\x86\x97", g->repo, g->pr);
    char *count = g->count > 1 ? xstrfmt("%zu finding%s across %zu reviews", findings, findings == 1 ? "" : "s", g->count) : xstrfmt("%zu finding%s", findings, findings == 1 ? "" : "s");
    int top = doc->y, h = px(22);
    int hw = text_width(doc->cv, heading, FONT_BODY_SEMIBOLD) + px(4);
    if (hw > w - px(80)) hw = w - px(80);
    RECT hr = { x, top, x + hw, top + h };
    int hi = doc_text_at(doc, &hr, heading, FONT_BODY_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    clickable(doc, hi, ACT_OPEN_PR, (intptr_t)g->rounds[0]);
    RECT cr = { hr.right + px(8), top, x + w, top + h };
    doc_text_at(doc, &cr, count, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = top + h + px(6);
    free(heading); free(count);
    for (size_t i = 0; i < g->count; i++) layout_round(s, doc, x, w, g->rounds[i]);
    doc_space(doc, px(6));
}

void screen_findings_findings_layout(Screen *base, Doc *doc) {
    FindingsScreen *s = (FindingsScreen *)base;
    s->doc_stale = false;
    int w = doc->width, x = 0, iw = w;
    doc_space(doc, px(14));
    if (s->error) { doc_notice(doc, x, iw, s->error); doc_space(doc, px(10)); }
    for (size_t i = 0; i < s->outcome_count; i++) layout_outcome(s, doc, x, iw, i);
    if (s->round_count) {
        Group *groups; size_t n = build_groups(s, &groups);
        for (size_t i = 0; i < n; i++) layout_group(s, doc, x, iw, &groups[i]);
        groups_free(groups, n);
    } else if (s->loaded) {
        doc_text(doc, x, iw, "No review is waiting. Findings arrive here from \xE2\x8C\x95 Code review and from every review-loop round.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    }
    if (!s->loaded) doc_loading(doc, 0, w, "Loading findings\xE2\x80\xA6");
    doc_space(doc, px(12));
}

void screen_findings_findings_header(Screen *base, HeaderInfo *info) {
    FindingsScreen *s = (FindingsScreen *)base;
    snprintf(info->title, sizeof info->title, "\xE2\x9A\x91 Findings");
    Group *groups; size_t n = build_groups(s, &groups);
    groups_free(groups, n);
    char *sub = findings_subtitle(s->round_count, n);
    snprintf(info->subtitle, sizeof info->subtitle, "%s", sub);
    free(sub);
}
