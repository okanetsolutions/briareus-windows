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

void set_string(char **slot, const char *value) { free(*slot); *slot = xstrdup(value); }

const char *finding_severity_label(const char *severity, COLORREF *color) {
    char *f = str_fold(severity);
    const char *label;
    if (str_eq(f, "critical")) { label = "CRIT"; *color = theme.danger; }
    else if (str_eq(f, "high")) { label = "HIGH"; *color = theme.danger; }
    else if (str_eq(f, "low")) { label = "LOW"; *color = theme.muted; }
    else { label = "MED"; *color = theme.warn; }
    free(f);
    return label;
}
const char *const finding_decision_ids[FINDING_DECISION_COUNT] = { "fix", "optional", "dismissed" };
const char *const finding_decision_titles[FINDING_DECISION_COUNT] = { "Fix", "Optional", "Dismiss" };
int finding_decision_index(const char *decision) {
    for (int k = 0; k < FINDING_DECISION_COUNT; k++) if (str_eq(decision, finding_decision_ids[k])) return k;
    return -1;
}

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
static void paint_dot(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { draw_status_dot(cv, rc->left + px(4), (rc->top + rc->bottom) / 2, ((DotData *)it->data)->status); }

int doc_session_row(Doc *doc, int x, int w, const Session *session, int action, intptr_t arg, bool selected, COLORREF background, int trailing) {
    int box = doc_box_begin(doc, x, w, px(7), selected ? theme.raise : background, selected ? theme.raise : background, px(8));
    int left = x + px(8), inner = w - px(16) - trailing;
    int top = doc->y, lh = px(23);
    RECT dr = { left, top, left + px(8), top + lh };
    int di = doc_add(doc, &dr, paint_dot);
    DotData *dd = xcalloc(1, sizeof *dd); dd->status = xstrdup(session_status(session));
    doc_item(doc, di)->data = dd; doc_item(doc, di)->free_data = dot_free;
    RECT tr = { left + px(7) + px(7), top, left + inner, top + lh };
    doc_text_at(doc, &tr, session_display_title(session), FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = top + lh + px(2);
    char *sub = session_subtitle(session);
    doc_text(doc, left, inner, sub, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
    free(sub);
    doc_box_end(doc, box, px(7));
    doc_box_action(doc, box, action, arg);
    return box;
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
    else if (strstr(n, "todo") || strstr(n, "plan")) g = 0xE9D5;
    else if (strstr(n, "edit") || strstr(n, "write") || strstr(n, "patch")) g = 0xE70F;
    else if (strstr(n, "grep") || strstr(n, "glob") || strstr(n, "search") || strstr(n, "find")) g = 0xE721;
    else if (strstr(n, "web") || strstr(n, "fetch")) g = 0xE774;
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
static void paint_linked(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    LinkedData *d = it->data;
    int x = rc->left;
    RECT g = { x, rc->top, x + px(12), rc->bottom }; draw_text(cv, "\xE2\x86\xB3", &g, FONT_CAPTION, theme.tertiary, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += px(16);
    int rw = text_width(cv, d->reference, FONT_MONO_CAPTION2);
    RECT r = { x, rc->top, x + rw, rc->bottom }; draw_text(cv, d->reference, &r, FONT_MONO_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += rw + px(5);
    int sw = d->state ? text_width(cv, d->state, FONT_CAPTION2) + px(6) : 0;
    RECT t = { x, rc->top, rc->right - sw, rc->bottom };
    draw_text(cv, d->title, &t, FONT_CAPTION, it->action ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (d->state) {
        int tw = text_width(cv, d->title, FONT_CAPTION);
        int sx = x + (tw < rc->right - sw - x ? tw : rc->right - sw - x) + px(6);
        RECT s = { sx, rc->top, sx + sw, rc->bottom };
        draw_text(cv, d->state, &s, FONT_CAPTION2, d->state_color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}
void doc_linked_row(Doc *doc, int x, int w, const BoardLink *link, const char *repo, int action, intptr_t arg) {
    LinkedData *d = xcalloc(1, sizeof *d);
    d->reference = board_link_reference(link, repo); d->title = xstrdup(link->title);
    if (link->draft) { d->state = xstrdup("draft"); d->state_color = theme.warning; }
    else if (board_link_not_planned(link)) { d->state = xstrdup("not planned"); d->state_color = theme.warning; }
    else if (str_eq(link->state, "open")) { d->state = xstrdup("open"); d->state_color = theme.success; }
    else if (str_eq(link->state, "closed")) { d->state = xstrdup("closed"); d->state_color = theme.secondary; }
    int h = px(20);
    int i = doc_custom(doc, x, w, h, paint_linked, d, linked_free, action, arg);
    doc_item(doc, i)->color = theme.ink;
}

/// One 12px muted line of facts, `·` between them, the last one pushed to the right edge.
typedef struct { char **parts; COLORREF *colors; bool *mono; size_t count; char *right; } MetaData;
static void meta_free(void *p) { MetaData *d = p; str_array_free(d->parts, d->count); free(d->colors); free(d->mono); free(d->right); free(d); }
static void paint_meta(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    MetaData *d = it->data;
    int x = rc->left, right = rc->right;
    if (d->right) { int rw = text_width(cv, d->right, FONT_CAPTION); RECT rr = { right - rw, rc->top, right, rc->bottom }; draw_text(cv, d->right, &rr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); right -= rw + px(8); }
    int dot_w = text_width(cv, "\xC2\xB7", FONT_CAPTION);
    for (size_t i = 0; i < d->count && x < right; i++) {
        if (i) { RECT dr = { x + px(6), rc->top, x + px(6) + dot_w, rc->bottom }; draw_text(cv, "\xC2\xB7", &dr, FONT_CAPTION, theme.line, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += px(6) * 2 + dot_w; }
        FontId f = d->mono[i] ? FONT_MONO_CAPTION2 : FONT_CAPTION;
        int w = text_width(cv, d->parts[i], f);
        if (x + w > right) w = right - x;
        RECT pr = { x, rc->top, x + w, rc->bottom };
        draw_text(cv, d->parts[i], &pr, f, d->colors[i], DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        x += w;
    }
}
typedef struct { MetaData *d; } MetaBuilder;
static void meta_add(MetaData *d, const char *text, COLORREF color, bool mono) {
    d->parts = xrealloc(d->parts, (d->count + 1) * sizeof *d->parts); d->colors = xrealloc(d->colors, (d->count + 1) * sizeof *d->colors); d->mono = xrealloc(d->mono, (d->count + 1) * sizeof *d->mono);
    d->parts[d->count] = xstrdup(text); d->colors[d->count] = color; d->mono[d->count] = mono; d->count++;
}
static void doc_meta(Doc *doc, int x, int w, MetaData *d) { doc_custom(doc, x, w, px(20), paint_meta, d, meta_free, 0, 0); }

void doc_pull_row(Doc *doc, int x, int w, const PullSummary *pull, const StackPosition *stack, const char *repo, int action, intptr_t arg, const ButtonSpec *buttons, size_t button_count, bool running) {
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, running ? theme.accent_dim : theme.line, px(12));
    int ix = x + px(12), iw = w - px(24);
    doc_text(doc, ix, iw, pull->title, FONT_BODY_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(4));
    MetaData *d = xcalloc(1, sizeof *d);
    char *number = xstrfmt("#%d", pull->number); meta_add(d, number, theme.muted, false); free(number);
    if (pull->draft) meta_add(d, "draft", theme.muted, false);
    if (pull_has_conflicts(pull)) meta_add(d, "\xE2\x9A\xA0 conflicts", theme.danger, false);
    if (pull->checks) {
        if (str_eq(pull->checks, "success")) meta_add(d, "\xE2\x9C\x93 checks", theme.ok, false);
        else if (str_eq(pull->checks, "failure") || str_eq(pull->checks, "error")) meta_add(d, "\xE2\x9C\x97 checks", theme.danger, false);
        else meta_add(d, "\xE2\x80\xA6 checks", theme.warn, false);
    }
    ReviewStatus review = review_status_of_reviewers(pull->review_decision, pull->reviewers, pull->reviewer_count);
    if (review != REVIEW_NONE) { COLORREF c; review_glyph(review, &c); meta_add(d, review_status_text(review), c, false); }
    if (stack) { char *label = stack_position_label(stack, 0); char *text = xstrfmt("stack %s", label); meta_add(d, text, theme.accent, false); free(text); free(label); }
    if (pull->assignee_count) { char *a = people(pull->assignees, pull->assignee_count, 2); meta_add(d, a, theme.muted, false); free(a); }
    else meta_add(d, "unassigned", theme.tertiary, false);
    if (pull->author) { char *a = xstrfmt("@%s", pull->author); meta_add(d, a, theme.muted, false); free(a); }
    if (!str_empty(pull->branch)) meta_add(d, pull->branch, theme.muted, true);
    if (pull->has_updated) d->right = format_relative(pull->updated_at);
    doc_meta(doc, ix, iw, d);
    if (pull->label_count) { doc_space(doc, px(6)); doc_label_chips(doc, ix, iw, pull->labels, pull->label_count, theme.raise); }
    for (size_t i = 0; i < pull->issue_count; i++) { doc_space(doc, px(4)); doc_linked_row(doc, ix, iw, &pull->issues[i], repo, 0, 0); }
    if (button_count) { doc_space(doc, px(8)); doc_button_row(doc, ix, iw, buttons, button_count); }
    doc_box_end(doc, box, px(10));
    doc_box_action(doc, box, action, arg);
}

typedef struct { int done, total; } EpicData;
static void paint_epic(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    EpicData *d = it->data;
    int bar_w = px(70), h = px(6);
    RECT track = { rc->left, rc->top + (rc->bottom - rc->top) / 2 - h / 2, rc->left + bar_w, rc->top + (rc->bottom - rc->top) / 2 + h / 2 };
    fill_round_rect(cv, &track, h / 2, blend(theme.accent, theme.elevated, 0.15), blend(theme.accent, theme.elevated, 0.15));
    int total = d->total > 0 ? d->total : 1;
    RECT fill = track; fill.right = track.left + bar_w * (d->done > total ? total : d->done) / total;
    if (fill.right > fill.left) fill_round_rect(cv, &fill, h / 2, theme.accent, theme.accent);
    char *text = xstrfmt("%d/%d done", d->done, d->total);
    RECT t = { rc->left + bar_w + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, text, &t, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    free(text);
}
void doc_epic_progress(Doc *doc, int x, int w, const IssueSummary *issue) {
    EpicData *d = xcalloc(1, sizeof *d); d->done = issue->sub_issues_done; d->total = issue->sub_issues;
    doc_custom(doc, x, w, font_height(doc->cv, FONT_CAPTION2) + px(4), paint_epic, d, free, 0, 0);
}

void doc_issue_row(Doc *doc, int x, int w, const IssueSummary *issue, const char *repo, bool nested, int action, intptr_t arg) {
    int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(12));
    int ix = x + px(12), iw = w - px(24);
    char *title = xstrfmt("%s%s", issue_is_epic(issue) ? "\xE2\x97\x8E " : "", issue->title);
    doc_text(doc, ix, iw, title, FONT_BODY_SEMIBOLD, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title);
    doc_space(doc, px(4));
    MetaData *d = xcalloc(1, sizeof *d);
    char *number = xstrfmt("#%d", issue->number); meta_add(d, number, theme.muted, false); free(number);
    if (issue->author) { char *a = xstrfmt("@%s", issue->author); meta_add(d, a, theme.muted, false); free(a); }
    if (issue->assignee_count) { char *a = people(issue->assignees, issue->assignee_count, 2); meta_add(d, a, theme.muted, false); free(a); }
    else meta_add(d, "unassigned", theme.tertiary, false);
    if (issue->comments > 0) { char *c = xstrfmt("%d comment%s", issue->comments, issue->comments == 1 ? "" : "s"); meta_add(d, c, theme.muted, false); free(c); }
    if (issue->milestone) meta_add(d, issue->milestone, theme.muted, false);
    if (issue->pull_count) meta_add(d, issue->pull_count == 1 ? "1 pull request" : "pull requests", theme.ok, false);
    if (issue->has_updated) d->right = format_relative(issue->updated_at);
    doc_meta(doc, ix, iw, d);
    if (issue_is_epic(issue)) { doc_space(doc, px(4)); doc_epic_progress(doc, ix, iw, issue); }
    if (issue->label_count) { doc_space(doc, px(6)); doc_label_chips(doc, ix, iw, issue->labels, issue->label_count, theme.raise); }
    if (!nested && issue->has_parent) {
        char *ref = board_link_reference(&issue->parent, repo);
        char *text = xstrfmt("Part of %s %s", ref, issue->parent.title);
        doc_space(doc, px(4));
        doc_text(doc, ix, iw, text, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
        free(text); free(ref);
    }
    for (size_t i = 0; i < issue->pull_count; i++) { doc_space(doc, px(4)); doc_linked_row(doc, ix, iw, &issue->pulls[i], repo, 0, 0); }
    doc_box_end(doc, box, px(10));
    doc_box_action(doc, box, action, arg);
}

// MARK: - Tabs

// GitHub's `tabnav`, as the pull request page and the project settings lay their tabs out.
typedef struct { wchar_t glyph; char *title, *count; bool active; } TabnavData;
static void tabnav_free(void *p) { TabnavData *d = p; free(d->title); free(d->count); free(d); }
static int tabnav_width(Canvas *cv, const TabnavData *d) {
    int w = px(12) + px(16) + px(8) + text_width(cv, d->title, FONT_FOOTNOTE_SEMIBOLD) + px(12);
    if (d->count) w += px(6) + text_width(cv, d->count, FONT_CAPTION) + px(12);
    return w;
}
static void paint_tabnav(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabnavData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF ink = d->active || hovered ? theme.ink : theme.muted;
    if (hovered && !d->active) { RECT h = { rc->left + px(2), rc->top + px(5), rc->right - px(2), rc->bottom - px(7) }; fill_round_rect(cv, &h, px(6), theme.raise, theme.raise); }
    int x = rc->left + px(12);
    RECT g = { x, rc->top, x + px(16), rc->bottom - px(2) }; draw_glyph(cv, d->glyph, &g, FONT_ICON_SMALL, d->active ? theme.ink : theme.muted); x += px(16) + px(8);
    FontId f = d->active ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE;
    int lw = text_width(cv, d->title, f);
    RECT t = { x, rc->top, x + lw + px(2), rc->bottom - px(2) }; draw_text(cv, d->title, &t, f, ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE); x += lw + px(6);
    if (d->count) {
        // `.Counter`: the number in a small rounded bubble.
        int cw = text_width(cv, d->count, FONT_CAPTION) + px(12), ch = font_height(cv, FONT_CAPTION) + px(4);
        int cy = rc->top + (rc->bottom - px(2) - rc->top - ch) / 2;
        RECT b = { x, cy, x + cw, cy + ch }; fill_round_rect(cv, &b, ch / 2, theme.line, theme.line);
        draw_text(cv, d->count, &b, FONT_CAPTION, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (d->active) { RECT u = { rc->left + px(4), rc->bottom - px(2), rc->right - px(4), rc->bottom }; fill_round_rect(cv, &u, px(1), theme.accent, theme.accent); }
}
void doc_tab(Doc *doc, int *x, int *y, int left, int right, int h, wchar_t glyph, const char *title, const char *count, bool active, int action, intptr_t arg) {
    TabnavData *d = xcalloc(1, sizeof *d); d->glyph = glyph; d->title = xstrdup(title); d->count = xstrdup(count); d->active = active;
    int w = tabnav_width(doc->cv, d);
    if (*x > left && *x + w > right) { *x = left; *y += h; }
    doc->y = *y;
    int i = doc_custom(doc, *x, w, h, paint_tabnav, d, tabnav_free, active ? 0 : action, arg);
    doc_item(doc, i)->hover_fill = false;
    *x += w;
}
int content_left(Pane *pane) { (void)pane; return 0; }
