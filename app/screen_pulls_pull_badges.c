#include "screen_pulls_internal.h"

/// `.prv-state`: a filled pill, capitalised, green for open, the accent for merged, red for closed, grey for a draft.
typedef struct { char *text; COLORREF color; } PillData;

/// One pull request of the overview: the pull request glyph, its title, and `#number · branch` under it, as GitHub's popover rows.
typedef struct { char *title, *sub; bool current; } StackRowData;

/// The base the bottom merges into, as GitHub closes its popover: a hollow dot and the branch in a chip.
typedef struct { char *base; } StackBaseData;

static void pill_free(void *p);
static void paint_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_stack_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void stack_row_free(void *p);
static void paint_stack_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void stack_base_free(void *p);
static void paint_stack_base(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void flow_push(FlowData *d, const char *text, size_t n, FontId font, COLORREF color, bool chip);
static void paint_flow(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

void screen_pulls_paint_counts(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
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

int screen_pulls_section_box(Doc *doc, int w) { int b = doc_box_begin(doc, 0, w, px(12), theme.raise, theme.line, px(8)); doc_item(doc, b)->hover_fill = false; return b; }

Col screen_pulls_col_make(int x, int w) { Col c = { x, w, x + px(12), w - px(24) }; return c; }

int screen_pulls_col_box(Doc *doc, Col c) { int b = doc_box_begin(doc, c.x, c.w, px(12), theme.raise, theme.line, px(8)); doc_item(doc, b)->hover_fill = false; return b; }

static void pill_free(void *p) { PillData *d = p; free(d->text); free(d); }

static void paint_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    PillData *d = it->data;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, d->color, d->color);
    RECT t = *rc;
    draw_text(cv, d->text, &t, FONT_FOOTNOTE_SEMIBOLD, theme.canvas, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

int screen_pulls_doc_pill(Doc *doc, int x, int y, const char *text, COLORREF color) {
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

int screen_pulls_doc_stack_pill(Doc *doc, int x, int y, const char *text, bool open) {
    PillData *d = xcalloc(1, sizeof *d); d->text = xstrdup(text); d->color = theme.accent;
    int w = px(10) * 2 + px(16) + px(4) + text_width(doc->cv, text, FONT_FOOTNOTE_SEMIBOLD) + 2, h = px(26);
    RECT rc = { x, y, x + w, y + h };
    int i = doc_add(doc, &rc, paint_stack_pill);
    Item *it = doc_item(doc, i);
    it->data = d; it->free_data = pill_free; it->action = ACT_STACK_TOGGLE; it->arg = open; it->hand = true;
    return w;
}

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
void screen_pulls_layout_stack_overview(PullScreen *s, Doc *doc, Col c) {
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

void screen_pulls_flow_free(void *p) { FlowData *d = p; for (size_t i = 0; i < d->count; i++) free(d->runs[i].text); free(d->runs); free(d); }

FlowData *screen_pulls_flow_new(int line_h) { FlowData *d = xcalloc(1, sizeof *d); d->line_h = line_h; return d; }

static void flow_push(FlowData *d, const char *text, size_t n, FontId font, COLORREF color, bool chip) {
    d->runs = xrealloc(d->runs, (d->count + 1) * sizeof *d->runs);
    FlowRun r = { xstrndup(text, n), font, color, chip, 0, 0, 0 };
    d->runs[d->count++] = r;
}

/// Plain text goes in word by word, each word keeping its trailing space; a chip goes in whole.
void screen_pulls_flow_add(FlowData *d, const char *text, FontId font, COLORREF color, bool chip) {
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
int screen_pulls_doc_flow(Doc *doc, int x, int w, int indent, FlowData *d) {
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
    doc_item(doc, i)->data = d; doc_item(doc, i)->free_data = screen_pulls_flow_free;
    doc->y = rc.bottom;
    return i;
}

void screen_pulls_paint_event_badge(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    EventBadge *d = it->data;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, theme.raise, theme.line);
    draw_glyph(cv, d->glyph, rc, FONT_ICON_SMALL, d->color);
}

void screen_pulls_flow_strong(FlowData *f, const char *text) { if (text) { char *t = xstrfmt("%s ", text); screen_pulls_flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.ink, false); free(t); } }

void screen_pulls_flow_muted(FlowData *f, const char *text) { screen_pulls_flow_add(f, text, FONT_FOOTNOTE, theme.muted, false); }

/// An issue or pull request an event points to: `#12 Its title`, or `owner/name#12` from another repository.
void screen_pulls_flow_ref(FlowData *f, const Json *ref, const char *repo) {
    BoardLink link;
    if (!board_link_parse(ref, &link)) return;
    char *r = board_link_reference(&link, repo), *t = xstrfmt("%s ", r);
    screen_pulls_flow_add(f, t, FONT_FOOTNOTE_SEMIBOLD, theme.accent, false);
    char *title = xstrfmt("%s ", link.title); screen_pulls_flow_add(f, title, FONT_FOOTNOTE, theme.ink, false);
    free(title); free(t); free(r);
    board_link_free(&link);
}
