#include "screen_projects_internal.h"

// MARK: - Rows

typedef struct { char *name; int count; bool busy, selected, chevron; } ProjectRowData;

/// A conversation as the dashboard lists it: its mark, title, and a line of provider, branch, state and age.
typedef struct {
    char *title, *repo, *provider, *branch, *state, *ago, *pr_state;
    bool lit, select_mode, picked, orchestrator, zeus;
    int meta_h;
} SessionRowData;

static void project_row_free(void *p);
static void paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void session_row_free(void *p);
static int chip_w(Canvas *cv, const char *text);
static int meta_layout(Canvas *cv, SessionRowData *d, int width, RECT *rects);
static void draw_pr_mark(Canvas *cv, int x, int y, COLORREF color);
static void paint_session_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static const char *session_state(const Session *s);
static char *session_age(const Session *s);

static void project_row_free(void *p) { ProjectRowData *d = p; free(d->name); free(d); }

static void paint_project_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ProjectRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (hovered || d->selected) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    int right = rc->right - px(8);
    if (d->chevron) { RECT c = { right - px(6), rc->top, right, rc->bottom }; draw_text(cv, "\xE2\x80\xBA", &c, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE); right -= px(6) + px(8); }
    char n[16]; snprintf(n, sizeof n, "%d", d->count);
    int nw = text_width(cv, n, FONT_CAPTION);
    RECT cr = { right - nw, rc->top, right, rc->bottom }; draw_text(cv, n, &cr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    right -= nw + px(8);
    if (d->busy) { draw_status_dot(cv, right - px(4), (rc->top + rc->bottom) / 2, "running"); right -= px(7) + px(8); }
    RECT t = { rc->left + px(8), rc->top, right, rc->bottom };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_projects_doc_project_row(Doc *doc, int w, const char *name, int count, bool busy, bool selected, bool chevron, int h, int action, intptr_t arg) {
    ProjectRowData *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(name); d->count = count; d->busy = busy; d->selected = selected; d->chevron = chevron;
    doc_custom(doc, 0, w, h, paint_project_row, d, project_row_free, action, arg);
}

void screen_projects_paint_back_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    COLORREF c = hovered ? theme.ink : theme.muted;
    RECT a = { rc->left + px(8), rc->top, rc->left + px(8) + px(8), rc->bottom };
    draw_text(cv, "\xE2\x80\xB9", &a, FONT_FOOTNOTE, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT t = { a.right + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "All projects", &t, FONT_CAPTION, c, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void session_row_free(void *p) { SessionRowData *d = p; free(d->title); free(d->repo); free(d->provider); free(d->branch); free(d->state); free(d->ago); free(d->pr_state); free(d); }

static int chip_w(Canvas *cv, const char *text) { return px(5) * 2 + text_width(cv, text, FONT_CAPTION2) + 2; }

/// Lays the metadata chips out at `width`, wrapping as `flex-wrap` does; NULL cv rects only measure. Returns the height.
static int meta_layout(Canvas *cv, SessionRowData *d, int width, RECT *rects) {
    // provider chip, branch chip (at most 45% wide), state, age; `gap-2` between them, 20px lines.
    int lh = px(20), gap = px(8), x = 0, y = 0;
    const char *branch = d->orchestrator ? (d->zeus ? "\xE2\x9A\xA1 zeus" : "\xF0\x9F\xA7\xAD orchestrator") : d->branch;
    int widths[4] = { chip_w(cv, d->provider), branch && *branch ? chip_w(cv, branch) : 0, text_width(cv, d->state, FONT_CAPTION), text_width(cv, d->ago, FONT_CAPTION) };
    if (widths[1] > width * 45 / 100) widths[1] = width * 45 / 100;
    for (int i = 0; i < 4; i++) {
        if (!widths[i]) { SetRectEmpty(&rects[i]); continue; }
        // The state and its age wrap as one, so "waiting" never sits a line above "11h ago".
        int need = widths[i] + (i == 2 && widths[3] ? gap + widths[3] : 0);
        bool joined = i == 3 && !IsRectEmpty(&rects[2]);
        if (x > 0 && !joined && x + need > width) { x = 0; y += lh; }
        RECT r = { x, y, x + widths[i], y + lh }; rects[i] = r;
        x += widths[i] + gap;
    }
    return y + lh;
}

/// GitHub's pull request mark, drawn in 13px: a branch with a commit at each end and the merge ring beside it.
static void draw_pr_mark(Canvas *cv, int x, int y, COLORREF color) {
    int s = px(13);
    int w = px(1) + 1;
    int lx = x + s * 3 / 13, top = y + s * 3 / 13, bottom = y + s * 11 / 13, rx = x + s * 10 / 13;
    draw_thick_line(cv, lx, top, lx, bottom, color, w);
    draw_thick_line(cv, rx, bottom, rx, y + s * 5 / 13, color, w);
    draw_thick_line(cv, rx, y + s * 4 / 13, x + s * 6 / 13, y + s * 4 / 13, color, w);
    int r = s * 2 / 13 + 1;
    fill_circle(cv, lx, top, r, color); fill_circle(cv, lx, bottom, r, color); fill_circle(cv, rx, bottom, r, color);
}

static void paint_session_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SessionRowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (hovered || d->lit) fill_round_rect(cv, rc, px(8), theme.raise, theme.raise);
    int x = rc->left + px(8), top = rc->top + px(7);
    if (d->select_mode) {
        RECT box = { x, top + px(4), x + px(13), top + px(4) + px(13) };
        fill_round_rect(cv, &box, px(2), d->picked ? theme.accent : theme.field, d->picked ? theme.accent : theme.line_strong);
        if (d->picked) draw_check_mark(cv, &box, theme.on_accent);
        x += px(13) + px(8);
    }
    x += px(8) + px(8);   // the fold gutter and the gap after it
    int right = rc->right - px(8), line_h = px(23);
    // Line one: the mark and the title.
    RECT l1 = { x, top, right, top + line_h };
    int mark_w;
    if (d->pr_state) {
        COLORREF c = str_eq(d->pr_state, "open") ? theme.ok : str_eq(d->pr_state, "merged") ? theme.accent : str_eq(d->pr_state, "closed") ? theme.danger : theme.muted;
        draw_pr_mark(cv, x, top + (line_h - px(13)) / 2, c);
        mark_w = px(13);
    } else { draw_status_dot(cv, x + px(3), top + line_h / 2, d->state); mark_w = px(7); }
    l1.left = x + mark_w + px(7);
    draw_text(cv, d->title, &l1, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Line two: the chips, wrapped as they were measured.
    RECT rects[4];
    meta_layout(cv, d, right - x, rects);
    int my = top + line_h + px(2);
    if (d->repo) {
        RECT r = { x, my, right, my + px(18) };
        draw_text(cv, d->repo, &r, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        my += px(18);
    }
    const char *branch = d->orchestrator ? (d->zeus ? "\xE2\x9A\xA1 zeus" : "\xF0\x9F\xA7\xAD orchestrator") : d->branch;
    const char *texts[4] = { d->provider, branch, d->state, d->ago };
    for (int i = 0; i < 4; i++) {
        if (IsRectEmpty(&rects[i])) continue;
        RECT r = { x + rects[i].left, my + rects[i].top, x + rects[i].right, my + rects[i].bottom };
        if (i < 2) {
            RECT chip = { r.left, r.top + px(1), r.right, r.bottom - px(1) };
            fill_round_rect(cv, &chip, px(4), hovered || d->lit ? theme.raise : theme.sidebar, theme.line);
            RECT t = { chip.left + px(5), chip.top, chip.right - px(5) + 2, chip.bottom };
            draw_text(cv, texts[i], &t, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else draw_text(cv, texts[i], &r, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

/// The dashboard's `sessionState`: an idle conversation with a question up is "waiting".
static const char *session_state(const Session *s) {
    if (str_eq(session_status(s), "idle") && json_bool_is(json_get(s->raw, "awaitingAnswer"), true)) return "waiting";
    return session_status(s);
}

static char *session_age(const Session *s) {
    time_t when;
    if (!board_date_parse(json_str(json_get(s->raw, "createdAt")), &when)) return xstrdup("");
    return format_relative(when);
}

void screen_projects_doc_dashboard_session_row(Doc *doc, int w, const Session *s, bool show_repo, bool lit, bool select_mode, bool picked, int action, intptr_t arg) {
    SessionRowData *d = xcalloc(1, sizeof *d);
    d->title = xstrdup(session_display_title(s));
    if (show_repo) d->repo = xstrdup(session_repo(s));
    d->provider = xstrdup(session_provider(s) ? session_provider(s) : "");
    d->branch = xstrdup(json_str_nonempty(json_get(s->raw, "branch")) ? json_str(json_get(s->raw, "branch")) : "");
    d->state = xstrdup(session_state(s)); d->ago = session_age(s);
    d->orchestrator = json_bool_is(json_get(s->raw, "orchestrator"), true) || json_bool_is(json_get(s->raw, "zeus"), true);
    d->zeus = json_bool_is(json_get(s->raw, "zeus"), true);
    const Json *pr = json_get(s->raw, "prStatus");
    const char *pr_state = json_str(json_get(pr, "state"));
    if (pr_state) d->pr_state = xstrdup(str_eq(pr_state, "open") && json_bool_is(json_get(pr, "draft"), true) ? "draft" : pr_state);
    d->lit = lit; d->select_mode = select_mode; d->picked = picked;
    int x = px(8) + (select_mode ? px(13) + px(8) : 0) + px(16);
    RECT rects[4];
    d->meta_h = meta_layout(doc->cv, d, w - x - px(8), rects);
    int h = px(7) + px(23) + px(2) + (d->repo ? px(18) : 0) + d->meta_h + px(7);
    doc_custom(doc, 0, w, h, paint_session_row, d, session_row_free, action, arg);
}
