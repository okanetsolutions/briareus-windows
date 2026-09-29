#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - Polling

void poller_start(Poller *p, Pane *pane, UINT timer_id, int base_ms) {
    p->pane = pane; p->timer_id = timer_id; p->base_ms = base_ms; p->running = true;
    SetTimer(pane_hwnd(pane), timer_id, 1, NULL);
}
void poller_stop(Poller *p) { if (p->pane && p->running) KillTimer(pane_hwnd(p->pane), p->timer_id); p->running = false; }
void poller_finished(Poller *p, bool failed, double retry_after) {
    if (failed) p->failures++; else p->failures = 0;
    p->retry_after = failed ? retry_after : -1;
    if (!p->running) return;
    SetTimer(pane_hwnd(p->pane), p->timer_id, (UINT)poll_delay_ms(p->base_ms, p->failures, p->retry_after), NULL);
}
bool poller_fired(Poller *p, UINT id) {
    if (!p->running || id != p->timer_id) return false;
    KillTimer(pane_hwnd(p->pane), p->timer_id);
    // Polling pauses while the app is in the background and resumes with it.
    if (!g_store.active) { SetTimer(pane_hwnd(p->pane), p->timer_id, 2000, NULL); return false; }
    return true;
}
void poller_set_base(Poller *p, int base_ms) { p->base_ms = base_ms; }

void set_string(char **slot, const char *value) { free(*slot); *slot = value ? xstrdup(value) : NULL; }

// MARK: - Rows

char *session_subtitle(const Session *session) {
    char *status = str_capitalized(session_status(session));
    const char *model = session_model(session);
    char *out = model ? xstrfmt("%s \xC2\xB7 %s", status, model) : xstrdup(status);
    free(status);
    return out;
}

typedef struct { char *status; } DotData;
static void dot_free(void *p) { DotData *d = p; free(d->status); free(d); }
static void paint_dot(Doc *doc, Item *it, HDC hdc, const RECT *rc) { draw_status_dot(hdc, rc->left + px(6), rc->top + px(9), ((DotData *)it->data)->status); }

void doc_session_row(Doc *doc, int x, int w, const Session *session, int action, intptr_t arg, bool selected, COLORREF background) {
    COLORREF fill = selected ? blend(theme.accent, background, 0.16) : background;
    int box = doc_box_begin(doc, x, w, px(9), fill, selected ? blend(theme.accent, background, 0.3) : theme.border, px(10));
    int left = x + px(12), inner = w - px(24);
    int top = doc->y;
    RECT dr = { left, top, left + px(14), top + px(18) };
    int di = doc_add(doc, &dr, paint_dot);
    DotData *dd = xcalloc(1, sizeof *dd); dd->status = xstrdup(session_status(session));
    doc_item(doc, di)->data = dd; doc_item(doc, di)->free_data = dot_free;
    bool closed = str_eq(session_status(session), "closed");
    int text_x = left + px(18), text_w = inner - px(18);
    RECT tr = { text_x, top, text_x + text_w, top };
    int title_h = measure_text(doc->hdc, session_display_title(session), text_w, FONT_BODY_MEDIUM, DT_WORDBREAK);
    int two_lines = font_height(doc->hdc, FONT_BODY_MEDIUM) * 2 + px(2);
    if (title_h > two_lines) title_h = two_lines;
    tr.bottom = top + title_h;
    doc_text_at(doc, &tr, session_display_title(session), FONT_BODY_MEDIUM, closed ? theme.secondary : theme.text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    doc->y = top + title_h + px(3);
    char *sub = session_subtitle(session);
    doc_text(doc, text_x, text_w, sub, FONT_CAPTION, theme.secondary, DT_SINGLELINE | DT_END_ELLIPSIS);
    free(sub);
    doc_box_end(doc, box, px(9));
    doc_box_action(doc, box, action, arg);
}

COLORREF label_color(const PullLabel *label) {
    int rgb[3];
    return pull_label_rgb(label, rgb) ? RGB(rgb[0], rgb[1], rgb[2]) : theme.secondary;
}

wchar_t review_glyph(ReviewStatus status, COLORREF *color) {
    switch (status) {
    case REVIEW_APPROVED: *color = theme.success; return 0xE73E;
    case REVIEW_CHANGES_REQUESTED: *color = theme.danger; return 0xE711;
    case REVIEW_FEEDBACK: *color = theme.warning; return 0xE8BD;
    case REVIEW_REQUESTED: *color = theme.secondary; return 0xE823;
    default: *color = theme.secondary; return 0;
    }
}
wchar_t check_glyph(const char *result, COLORREF *color) {
    char *r = str_fold(result);
    wchar_t g;
    if (str_eq(r, "success") || str_eq(r, "passed") || str_eq(r, "neutral") || str_eq(r, "skipped")) { *color = theme.success; g = 0xE930; }
    else if (str_eq(r, "failure") || str_eq(r, "failed") || str_eq(r, "timed_out") || str_eq(r, "action_required") || str_eq(r, "error")) { *color = theme.danger; g = 0xEA39; }
    else if (str_eq(r, "cancelled") || str_eq(r, "stale")) { *color = theme.secondary; g = 0xE738; }
    else { *color = theme.warning; g = 0xE823; }
    free(r);
    return g;
}
wchar_t tool_glyph(const char *kind, const char *name, bool is_error) {
    if (is_error) return 0xE7BA;
    if (str_eq(kind, "cmd")) return 0xE756;
    if (str_eq(kind, "git")) return 0xE8AB;
    char *n = str_fold(name);
    wchar_t g = 0xE90F;
    if (strstr(n, "bash") || strstr(n, "shell") || strstr(n, "exec")) g = 0xE756;
    else if (strstr(n, "read") || strstr(n, "view")) g = 0xE8A5;
    else if (strstr(n, "edit") || strstr(n, "write") || strstr(n, "patch")) g = 0xE70F;
    else if (strstr(n, "grep") || strstr(n, "glob") || strstr(n, "search") || strstr(n, "find")) g = 0xE721;
    else if (strstr(n, "web") || strstr(n, "fetch")) g = 0xE774;
    else if (strstr(n, "todo") || strstr(n, "plan")) g = 0xE9D5;
    else if (strstr(n, "task") || strstr(n, "agent")) g = 0xE716;
    free(n);
    return g;
}

size_t pull_badges(const PullSummary *pull, const StackPosition *stack, BadgeSpec *out, size_t cap, char *stack_text, size_t stack_text_len) {
    size_t n = 0;
    if (pull_conflicting(pull) && n < cap) { BadgeSpec b = { 0xE7BA, "Conflicts", theme.danger, false }; out[n++] = b; }
    if (pull->checks && n < cap) {
        BadgeSpec b = { 0xE823, "Checks", theme.warning, false };
        if (str_eq(pull->checks, "success")) { b.glyph = 0xE73E; b.color = theme.success; }
        else if (str_eq(pull->checks, "failure") || str_eq(pull->checks, "error")) { b.glyph = 0xE711; b.color = theme.danger; }
        out[n++] = b;
    }
    if (stack && n < cap) {
        char *label = stack_position_label(stack, 0);
        snprintf(stack_text, stack_text_len, "Stack %s", label); free(label);
        BadgeSpec b = { 0xE81E, stack_text, theme.accent, false }; out[n++] = b;
    }
    ReviewStatus review = review_status_of_reviewers(pull->review_decision, pull->reviewers, pull->reviewer_count);
    if (review != REVIEW_NONE && n < cap) { COLORREF c; wchar_t g = review_glyph(review, &c); BadgeSpec b = { g, review_status_text(review), c, false }; out[n++] = b; }
    if (pull->draft && n < cap) { BadgeSpec b = { 0xE70F, "Draft", theme.secondary, false }; out[n++] = b; }
    return n;
}

void doc_label_chips(Doc *doc, int x, int w, const PullLabel *labels, size_t count, COLORREF background) {
    if (!count) return;
    BadgeSpec *specs = xcalloc(count, sizeof *specs);
    for (size_t i = 0; i < count; i++) { specs[i].text = labels[i].name; specs[i].color = label_color(&labels[i]); specs[i].chip = true; }
    doc_badges(doc, x, w, specs, count, background);
    free(specs);
}

char *people(char **logins, size_t count, size_t limit) {
    Str s; str_init(&s);
    for (size_t i = 0; i < count && i < limit; i++) str_appendf(&s, "%s@%s", i ? ", " : "", logins[i]);
    if (count > limit) str_appendf(&s, " +%zu", count - limit);
    return str_detach(&s);
}

typedef struct { char *reference, *title, *state; COLORREF state_color; } LinkedData;
static void linked_free(void *p) { LinkedData *d = p; free(d->reference); free(d->title); free(d->state); free(d); }
static void paint_linked(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    LinkedData *d = it->data;
    int x = rc->left;
    RECT g = { x, rc->top, x + px(14), rc->bottom }; draw_glyph(hdc, 0xE71B, &g, FONT_ICON_SMALL, theme.tertiary); x += px(18);
    int rw = text_width(hdc, d->reference, FONT_MONO_SMALL);
    RECT r = { x, rc->top, x + rw, rc->bottom }; draw_text(hdc, d->reference, &r, FONT_MONO_SMALL, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += rw + px(5);
    int sw = d->state ? text_width(hdc, d->state, FONT_CAPTION2) + px(6) : 0;
    RECT t = { x, rc->top, rc->right - sw, rc->bottom };
    draw_text(hdc, d->title, &t, FONT_CAPTION, it->color, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (d->state) {
        int tw = text_width(hdc, d->title, FONT_CAPTION);
        int sx = x + (tw < rc->right - sw - x ? tw : rc->right - sw - x) + px(6);
        RECT s = { sx, rc->top, sx + sw, rc->bottom };
        draw_text(hdc, d->state, &s, FONT_CAPTION2, d->state_color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}
void doc_linked_row(Doc *doc, int x, int w, const BoardLink *link, const char *repo, int action, intptr_t arg) {
    LinkedData *d = xcalloc(1, sizeof *d);
    d->reference = board_link_reference(link, repo); d->title = xstrdup(link->title);
    if (link->draft) { d->state = xstrdup("draft"); d->state_color = theme.warning; }
    else if (board_link_not_planned(link)) { d->state = xstrdup("not planned"); d->state_color = theme.warning; }
    else if (str_eq(link->state, "open")) { d->state = xstrdup("open"); d->state_color = theme.success; }
    else if (str_eq(link->state, "closed")) { d->state = xstrdup("closed"); d->state_color = theme.secondary; }
    int h = font_height(doc->hdc, FONT_CAPTION) + px(6);
    int i = doc_custom(doc, x, w, h, paint_linked, d, linked_free, action, arg);
    doc_item(doc, i)->color = theme.text;
}

typedef struct { wchar_t glyph; COLORREF color; } GlyphData;
static void paint_glyph_item(Doc *doc, Item *it, HDC hdc, const RECT *rc) { GlyphData *d = it->data; draw_glyph(hdc, d->glyph, rc, FONT_ICON, d->color); }
static void add_glyph(Doc *doc, int x, int y, wchar_t glyph, COLORREF color) {
    RECT rc = { x, y, x + px(18), y + px(20) };
    int i = doc_add(doc, &rc, paint_glyph_item);
    GlyphData *d = xcalloc(1, sizeof *d); d->glyph = glyph; d->color = color;
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = free;
}
static void updated_text(Doc *doc, int right, int y, bool has, time_t when) {
    if (!has) return;
    char *rel = format_relative(when);
    int w = text_width(doc->hdc, rel, FONT_CAPTION2);
    RECT rc = { right - w, y, right, y + font_height(doc->hdc, FONT_CAPTION2) + px(2) };
    doc_text_at(doc, &rc, rel, FONT_CAPTION2, theme.secondary, DT_RIGHT | DT_SINGLELINE);
    free(rel);
}

void doc_pull_row(Doc *doc, int x, int w, const PullSummary *pull, const StackPosition *stack, const char *repo, int action, intptr_t arg, const ButtonSpec *buttons, size_t button_count) {
    int box = doc_box_begin(doc, x, w, px(10), theme.elevated, theme.border, px(10));
    int left = x + px(12), inner = w - px(24);
    COLORREF tint = pull_conflicting(pull) ? theme.danger : pull->draft ? theme.secondary : theme.success;
    add_glyph(doc, left, doc->y, 0xE8AB, tint);
    int tx = left + px(26), tw = inner - px(26);
    doc_text(doc, tx, tw, pull->title, FONT_BODY_MEDIUM, theme.text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    doc_space(doc, px(4));
    int y = doc->y;
    char *number = xstrfmt("#%d", pull->number);
    int nw = text_width(doc->hdc, number, FONT_MONO_SMALL);
    RECT nr = { tx, y, tx + nw, y + font_height(doc->hdc, FONT_MONO_SMALL) + px(2) };
    doc_text_at(doc, &nr, number, FONT_MONO_SMALL, theme.secondary, DT_LEFT | DT_SINGLELINE);
    free(number);
    int rel_w = pull->has_updated ? px(60) : 0;
    RECT br = { tx + nw + px(6), y, tx + tw - rel_w, nr.bottom };
    doc_text_at(doc, &br, pull->branch, FONT_MONO_SMALL, theme.secondary, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    updated_text(doc, tx + tw, y, pull->has_updated, pull->updated_at);
    doc->y = nr.bottom + px(5);
    BadgeSpec badges[6]; char stack_text[32];
    size_t bn = pull_badges(pull, stack, badges, 6, stack_text, sizeof stack_text);
    if (bn) { doc_badges(doc, tx, tw, badges, bn, theme.elevated); doc_space(doc, px(5)); }
    if (pull->label_count) { doc_label_chips(doc, tx, tw, pull->labels, pull->label_count, theme.elevated); doc_space(doc, px(5)); }
    if (pull->author || pull->assignee_count || pull->reviewer_count) {
        Str who; str_init(&who);
        if (pull->author) str_appendf(&who, "by @%s", pull->author);
        char *assigned = people(pull->assignees, pull->assignee_count, 2);
        str_appendf(&who, "%s%s", who.len ? " \xC2\xB7 " : "", pull->assignee_count ? "assigned " : "unassigned");
        if (pull->assignee_count) str_appendz(&who, assigned);
        free(assigned);
        if (pull->reviewer_count) {
            str_appendz(&who, " \xC2\xB7 review ");
            for (size_t i = 0; i < pull->reviewer_count && i < 2; i++) {
                const char *mark = str_eq(pull->reviewers[i].state, "approved") ? "\xE2\x9C\x93" : str_eq(pull->reviewers[i].state, "changes_requested") ? "\xE2\x9C\x97"
                                 : str_eq(pull->reviewers[i].state, "requested") ? "\xE2\x97\x8B" : "\xE2\x9C\x8E";
                str_appendf(&who, "%s%s @%s", i ? ", " : "", mark, pull->reviewers[i].user);
            }
            if (pull->reviewer_count > 2) str_appendf(&who, " +%zu", pull->reviewer_count - 2);
        }
        doc_text(doc, tx, tw, who.data, FONT_CAPTION, theme.secondary, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
        str_free(&who);
    }
    for (size_t i = 0; i < pull->issue_count; i++) doc_linked_row(doc, tx, tw, &pull->issues[i], repo, 0, 0);
    if (button_count) { doc_space(doc, px(8)); doc_button_row(doc, tx, tw, buttons, button_count); }
    doc_box_end(doc, box, px(10));
    doc_box_action(doc, box, action, arg);
}

typedef struct { int done, total; } EpicData;
static void paint_epic(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    EpicData *d = it->data;
    int bar_w = px(70), h = px(6);
    RECT track = { rc->left, rc->top + (rc->bottom - rc->top) / 2 - h / 2, rc->left + bar_w, rc->top + (rc->bottom - rc->top) / 2 + h / 2 };
    fill_round_rect(hdc, &track, h / 2, blend(theme.accent, theme.elevated, 0.15), blend(theme.accent, theme.elevated, 0.15));
    int total = d->total > 0 ? d->total : 1;
    RECT fill = track; fill.right = track.left + bar_w * (d->done > total ? total : d->done) / total;
    if (fill.right > fill.left) fill_round_rect(hdc, &fill, h / 2, theme.accent, theme.accent);
    char *text = xstrfmt("%d/%d done", d->done, d->total);
    RECT t = { rc->left + bar_w + px(8), rc->top, rc->right, rc->bottom };
    draw_text(hdc, text, &t, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    free(text);
}
static void doc_epic_progress(Doc *doc, int x, int w, const IssueSummary *issue) {
    EpicData *d = xcalloc(1, sizeof *d); d->done = issue->sub_issues_done; d->total = issue->sub_issues;
    doc_custom(doc, x, w, font_height(doc->hdc, FONT_CAPTION2) + px(4), paint_epic, d, free, 0, 0);
}

void doc_issue_row(Doc *doc, int x, int w, const IssueSummary *issue, const char *repo, bool nested, int action, intptr_t arg) {
    int box = doc_box_begin(doc, x, w, px(10), theme.elevated, theme.border, px(10));
    int left = x + px(12), inner = w - px(24);
    add_glyph(doc, left, doc->y, issue_is_epic(issue) ? 0xE81E : 0xEA3A, issue->pull_count ? theme.accent : theme.success);
    int tx = left + px(26), tw = inner - px(26);
    doc_text(doc, tx, tw, issue->title, FONT_BODY_MEDIUM, theme.text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL);
    doc_space(doc, px(4));
    Str meta; str_init(&meta);
    str_appendf(&meta, "#%d", issue->number);
    if (issue->author) str_appendf(&meta, " \xC2\xB7 @%s", issue->author);
    if (issue->assignee_count) { char *a = people(issue->assignees, issue->assignee_count, 2); str_appendf(&meta, " \xC2\xB7 assigned %s", a); free(a); }
    else str_appendz(&meta, " \xC2\xB7 unassigned");
    if (issue->comments > 0) str_appendf(&meta, " \xC2\xB7 %d comment%s", issue->comments, issue->comments == 1 ? "" : "s");
    if (issue->milestone) str_appendf(&meta, " \xC2\xB7 %s", issue->milestone);
    int y = doc->y;
    int rel_w = issue->has_updated ? px(60) : 0;
    RECT mr = { tx, y, tx + tw - rel_w, y + font_height(doc->hdc, FONT_CAPTION) + px(2) };
    doc_text_at(doc, &mr, meta.data, FONT_CAPTION, theme.secondary, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    str_free(&meta);
    updated_text(doc, tx + tw, y, issue->has_updated, issue->updated_at);
    doc->y = mr.bottom + px(5);
    if (issue_is_epic(issue)) { doc_epic_progress(doc, tx, tw, issue); doc_space(doc, px(4)); }
    if (issue->label_count) { doc_label_chips(doc, tx, tw, issue->labels, issue->label_count, theme.elevated); doc_space(doc, px(4)); }
    if (!nested && issue->has_parent) {
        char *ref = board_link_reference(&issue->parent, repo);
        char *text = xstrfmt("Part of %s %s", ref, issue->parent.title);
        doc_label(doc, tx, tw, 0xE8AB, text, FONT_CAPTION, theme.secondary);
        free(text); free(ref);
    }
    for (size_t i = 0; i < issue->pull_count; i++) doc_linked_row(doc, tx, tw, &issue->pulls[i], repo, 0, 0);
    doc_box_end(doc, box, px(10));
    doc_box_action(doc, box, action, arg);
}

int content_left(Pane *pane) { (void)pane; return 0; }
