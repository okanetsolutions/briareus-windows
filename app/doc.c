#include "doc.h"
#include "canvas.h"
#include "str.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

static void rich_free(void *data);

// MARK: - Items

void doc_init(Doc *doc) { memset(doc, 0, sizeof *doc); doc->hover = -1; doc->pressed = -1; doc->sel_anchor.item = doc->sel_focus.item = -1; }
static void clear_items(Doc *doc) {
    for (size_t i = 0; i < doc->count; i++) {
        Item *it = &doc->items[i];
        if (it->free_data) it->free_data(it->data);
        if (it->sel_owned) rich_free(it->sel);
        free(it->text); free(it->tip);
    }
    doc->count = 0;
}
void doc_free(Doc *doc) { clear_items(doc); free(doc->items); free(doc->regions); doc_init(doc); }
void doc_begin(Doc *doc, Canvas *cv, int width) {
    clear_items(doc);
    doc->cv = cv; doc->width = width; doc->y = 0; doc->content_width = width; doc->hover = -1; doc->pressed = -1;
    doc->sticky_first = doc->sticky_last = 0; doc->sticky_limit = 0; doc->sticky_shift = 0;
    doc->pin_last = doc->pin_bottom = doc->pin_shift = 0;
    doc->regions_kept = doc->region_count; doc->region_count = 0;
    doc->drag_first = doc->drag_last = 0;
}
void doc_end(Doc *doc) { doc->cv = NULL; }
Item *doc_item(Doc *doc, int index) { return index >= 0 && (size_t)index < doc->count ? &doc->items[index] : NULL; }
bool doc_item_hovered(const Doc *doc, const Item *it) { return doc->hover >= 0 && &doc->items[doc->hover] == it; }
int doc_height(const Doc *doc) { return doc->y; }

int doc_add(Doc *doc, const RECT *rc, ItemPaint paint) {
    if (doc->count == doc->cap) { doc->cap = doc->cap ? doc->cap * 2 : 64; doc->items = xrealloc(doc->items, doc->cap * sizeof *doc->items); }
    Item *it = &doc->items[doc->count];
    memset(it, 0, sizeof *it);
    it->rc = *rc; it->paint = paint; it->color = theme.text; it->font = FONT_BODY;
    if (rc->right > doc->content_width) doc->content_width = rc->right;
    return (int)doc->count++;
}

// MARK: - Rich text

// A run is one word (with its trailing spaces) of one span on one line. `start` and `len` place it in the plain text.
typedef struct { int x, y, w, wt, h, pad, line; size_t start, len; FontId font; COLORREF color; bool code, link, strike; char *url; wchar_t *text; } Run;
struct Rich {
    Run *runs; size_t count, cap; int height;
    wchar_t *plain; size_t plain_len;   // the text without markup, what a selection copies
    int *line_y, *line_h; size_t *line_start; int lines;
    bool single;                        // a single line, clipped to its item
};
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };
static bool item_selection(Doc *doc, Item *it, size_t *from, size_t *to);

static FontId rich_font(FontId base, unsigned flags) {
    if (flags & SPAN_CODE) return (base == FONT_FOOTNOTE || base == FONT_CAPTION || base == FONT_CAPTION2) ? FONT_MONO_SMALL : FONT_MONO;
    if (flags & SPAN_BOLD) {
        switch (base) {
        case FONT_BODY: return FONT_BODY_SEMIBOLD;
        case FONT_CALLOUT: case FONT_SUBHEADLINE: return FONT_SUBHEADLINE_SEMIBOLD;
        case FONT_FOOTNOTE: return FONT_FOOTNOTE_SEMIBOLD;
        case FONT_CAPTION: return FONT_CAPTION_SEMIBOLD;
        default: return base;
        }
    }
    if (flags & SPAN_ITALIC) {
        switch (base) {
        case FONT_BODY: return FONT_BODY_ITALIC;
        case FONT_CALLOUT: return FONT_CALLOUT_ITALIC;
        case FONT_SUBHEADLINE: return FONT_SUBHEADLINE_ITALIC;
        default: return base;
        }
    }
    return base;
}

static void rich_free(void *data) {
    Rich *r = data;
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) { free(r->runs[i].text); free(r->runs[i].url); }
    free(r->runs); free(r->plain); free(r->line_y); free(r->line_h); free(r->line_start); free(r);
}
/// Moves every run, as when text sits after a glyph or is centred vertically.
static void rich_offset(Rich *r, int dx, int dy) {
    for (size_t i = 0; i < r->count; i++) { r->runs[i].x += dx; r->runs[i].y += dy; }
    for (int l = 0; l < r->lines; l++) r->line_y[l] += dy;
}
/// Width of a run's text without its trailing spaces.
static int trimmed_width(FontId f, const wchar_t *text, size_t len) {
    while (len && text[len - 1] == L' ') len--;
    return textw_extent(f, text, len);
}

static Run *rich_push(Rich *r) {
    if (r->count == r->cap) { r->cap = r->cap ? r->cap * 2 : 32; r->runs = xrealloc(r->runs, r->cap * sizeof *r->runs); }
    Run *run = &r->runs[r->count++];
    memset(run, 0, sizeof *run);
    return run;
}

typedef struct { FontId font; COLORREF color; bool code, link, strike; const char *url; int line_h, pad; } RunStyle;
static Run *run_add(Rich *r, const RunStyle *st, const wchar_t *p, size_t len, size_t start, int x, int line, int w) {
    Run *run = rich_push(r);
    run->x = x; run->line = line; run->w = w; run->h = st->line_h; run->pad = st->pad; run->font = st->font; run->color = st->color;
    run->code = st->code; run->link = st->link; run->strike = st->strike; run->url = st->link ? xstrdup(st->url) : NULL;
    run->start = start; run->len = len;
    run->text = xmalloc((len + 1) * sizeof(wchar_t)); memcpy(run->text, p, len * sizeof(wchar_t)); run->text[len] = 0;
    run->wt = trimmed_width(st->font, p, len) + st->pad * 2;
    return run;
}
static void line_begin(Rich *r, size_t *cap, int line, size_t offset) {
    if ((size_t)line >= *cap) { *cap = *cap ? *cap * 2 : 16; r->line_start = xrealloc(r->line_start, *cap * sizeof *r->line_start); }
    r->line_start[line] = offset;
}

/// Breaks the text into word runs that fit `width`, line by line. `literal` text keeps its characters; otherwise it is inline
/// Markdown. `gap` is the space under each line; `align` places the lines in `align_w` (the wrap width when 0).
static Rich *rich_layout(Canvas *cv, const char *source, int width, FontId base, COLORREF color, bool literal, int gap, int align, int align_w) {
    Rich *r = xcalloc(1, sizeof *r);
    size_t n = 1; MdSpan literal_span = { 0, (char *)source, NULL };
    MdSpan *spans = literal ? &literal_span : md_inline(source, &n);
    int x = 0, line = 0;
    int pad = px(3);   // code span padding
    size_t line_cap = 0;
    line_begin(r, &line_cap, 0, 0);
    r->plain = xmalloc(sizeof *r->plain); r->plain[0] = 0;
    for (size_t s = 0; s < n; s++) {
        RunStyle st; memset(&st, 0, sizeof st);
        st.font = rich_font(base, spans[s].flags);
        st.code = (spans[s].flags & SPAN_CODE) != 0; st.link = (spans[s].flags & SPAN_LINK) != 0 && spans[s].url; st.strike = (spans[s].flags & SPAN_STRIKE) != 0;
        st.color = st.link ? theme.accent : st.strike ? theme.muted : color;
        st.url = spans[s].url; st.pad = st.code ? pad : 0;
        wchar_t *text = utf8_to_wide(spans[s].text);
        size_t tlen = wcslen(text), base_off = r->plain_len;
        r->plain = xrealloc(r->plain, (r->plain_len + tlen + 1) * sizeof *r->plain);
        memcpy(r->plain + r->plain_len, text, (tlen + 1) * sizeof *text); r->plain_len += tlen;
        st.line_h = font_height(cv, st.font) + (st.code ? pad : 0);
        const wchar_t *p = text;
        while (*p) {
            if (*p == L'\n') { x = 0; line++; p++; line_begin(r, &line_cap, line, base_off + (size_t)(p - text)); continue; }
            // A word with its trailing spaces.
            const wchar_t *end = p;
            if (st.code) { while (*end && *end != L'\n') end++; }
            else { while (*end && *end != L' ' && *end != L'\n') end++; }
            const wchar_t *fit_end = end;
            while (*end == L' ') end++;
            size_t len = (size_t)(end - p), fit_len = (size_t)(fit_end - p);
            if (fit_len == 0 && len > 0) fit_len = len;
            int fit = textw_extent(st.font, p, fit_len), full = textw_extent(st.font, p, len);
            int extra = st.pad * 2;
            if (x > 0 && x + fit + extra > width) { x = 0; line++; line_begin(r, &line_cap, line, base_off + (size_t)(p - text)); }
            if (fit + extra > width && fit_len > 1) {
                // Break a word longer than the line by characters.
                size_t k = 1;
                for (;;) {
                    k = textw_fit(st.font, p, fit_len, width - extra);
                    if (k == 0) k = 1;
                    if (k >= fit_len) break;
                    run_add(r, &st, p, k, base_off + (size_t)(p - text), x, line, textw_extent(st.font, p, k) + extra);
                    p += k; fit_len -= k; len -= k; x = 0; line++; line_begin(r, &line_cap, line, base_off + (size_t)(p - text));
                }
                full = textw_extent(st.font, p, len);
            }
            Run *run = run_add(r, &st, p, len, base_off + (size_t)(p - text), x, line, full + extra);
            x += run->w;
            p = end;
        }
        free(text);
    }
    if (!literal) md_spans_free(spans, n);
    // Line heights, then run positions.
    r->lines = line + 1;
    r->line_y = xcalloc((size_t)r->lines, sizeof *r->line_y); r->line_h = xcalloc((size_t)r->lines, sizeof *r->line_h);
    int base_h = font_height(cv, base) + gap;
    for (int l = 0; l < r->lines; l++) r->line_h[l] = base_h;
    for (size_t i = 0; i < r->count; i++) if (r->runs[i].h + gap > r->line_h[r->runs[i].line]) r->line_h[r->runs[i].line] = r->runs[i].h + gap;
    int y = 0;
    for (int l = 0; l < r->lines; l++) {
        r->line_y[l] = y;
        for (size_t i = 0; i < r->count; i++) if (r->runs[i].line == l) r->runs[i].y = y + (r->line_h[l] - gap - r->runs[i].h);
        y += r->line_h[l];
    }
    r->height = y;
    if (align != ALIGN_LEFT) {
        int aw = align_w > 0 ? align_w : width;
        for (int l = 0; l < r->lines; l++) {
            int line_w = 0;
            for (size_t i = 0; i < r->count; i++) if (r->runs[i].line == l && r->runs[i].x + r->runs[i].wt > line_w) line_w = r->runs[i].x + r->runs[i].wt;
            int shift = align == ALIGN_CENTER ? (aw - line_w) / 2 : aw - line_w;
            if (shift > 0) for (size_t i = 0; i < r->count; i++) if (r->runs[i].line == l) r->runs[i].x += shift;
        }
    }
    return r;
}

static COLORREF selection_color(void) { return blend(theme.accent, theme.background, theme.dark ? 0.4 : 0.3); }
/// Tints the characters `from`...`to` of the plain text, line by line.
static void rich_highlight(Canvas *cv, const Rich *r, size_t from, size_t to, const RECT *rc, COLORREF color) {
    for (size_t i = 0; i < r->count; i++) {
        const Run *run = &r->runs[i];
        size_t s = from > run->start ? from : run->start, e = to < run->start + run->len ? to : run->start + run->len;
        if (s >= e) continue;
        int a = textw_extent(run->font, run->text, s - run->start), b = textw_extent(run->font, run->text, e - run->start);
        int x = rc->left + run->x + run->pad;
        RECT h = { x + a, rc->top + r->line_y[run->line], x + b, rc->top + r->line_y[run->line] + r->line_h[run->line] };
        if (r->single && h.right > rc->right) h.right = rc->right;
        if (h.right > h.left) fill_rect(cv, &h, color);
    }
}

/// Paints runs at `rc`, with the selection behind them; `it` may be NULL for text that is not an item (a table cell).
static void rich_paint(Doc *doc, Item *it, Rich *r, Canvas *cv, const RECT *rc) {
    if (!r) return;
    size_t from, to;
    if (it && item_selection(doc, it, &from, &to)) rich_highlight(cv, r, from, to, rc, selection_color());
    for (size_t i = 0; i < r->count; i++) {
        Run *run = &r->runs[i];
        RECT rr = { rc->left + run->x, rc->top + run->y, rc->left + run->x + run->w, rc->top + run->y + run->h };
        if (run->code) {
            // Trailing spaces stay outside the tint.
            RECT bg = { rr.left, rr.top, rr.left + run->wt, rr.bottom };
            fill_round_rect(cv, &bg, px(4), theme.sunken, theme.sunken);
        }
        rr.left += run->pad;
        if (r->single) {
            if (rr.left >= rc->right) continue;
            if (rr.right > rc->right) rr.right = rc->right;
            draw_textw(cv, run->text, &rr, run->font, run->color, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        } else draw_textw(cv, run->text, &rr, run->font, run->color, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP);
        int text_w = run->wt - run->pad * 2;
        if (run->strike) { int mid = rr.bottom - run->h / 2; draw_line(cv, rr.left, mid, rr.left + text_w, mid, run->color); }
        if (run->link) draw_line(cv, rr.left, rr.bottom - 1, rr.left + text_w, rr.bottom - 1, blend(theme.accent, theme.background, 0.6));
    }
}
static void paint_rich(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { rich_paint(doc, it, it->data, cv, rc); }
int doc_rich(Doc *doc, int x, int w, const char *markdown, FontId base, COLORREF color) {
    Rich *r = rich_layout(doc->cv, markdown ? markdown : "", w, base, color, false, px(4), ALIGN_LEFT, 0);
    RECT rc = { x, doc->y, x + w, doc->y + r->height };
    int i = doc_add(doc, &rc, paint_rich);
    Item *it = &doc->items[i];
    it->data = r; it->free_data = rich_free; it->sel = r; it->text = xstrdup(markdown ? markdown : "");
    bool has_link = false;
    for (size_t k = 0; k < r->count && !has_link; k++) has_link = r->runs[k].link;
    if (has_link) { it->action = ACTION_OPEN_LINK; it->hand = true; }
    doc->y += r->height;
    return i;
}
int doc_rich_height(Doc *doc, int w, const char *markdown, FontId base) {
    Rich *r = rich_layout(doc->cv, markdown ? markdown : "", w, base, theme.text, false, px(4), ALIGN_LEFT, 0);
    int h = r->height;
    rich_free(r);
    return h;
}
const char *doc_link_at(Doc *doc, int index, int x, int y) {
    Item *it = doc_item(doc, index);
    if (!it || it->paint != paint_rich) return NULL;
    Rich *r = it->data;
    for (size_t i = 0; i < r->count; i++) {
        Run *run = &r->runs[i];
        if (!run->url) continue;
        int rx = it->rc.left + run->x, ry = it->rc.top + run->y;
        if (x >= rx && x < rx + run->w && y >= ry && y < ry + run->h + px(3)) return run->url;
    }
    return NULL;
}

// MARK: - Text

/// Text with an ellipsis is drawn by DrawText and cannot be selected; everything else is laid out in runs.
static bool uses_runs(UINT flags) { return !(flags & (DT_END_ELLIPSIS | DT_PATH_ELLIPSIS | DT_WORD_ELLIPSIS)); }
static char *expand_tabs(const char *text) {
    size_t col = 0, cap = strlen(text) * 2 + 16, n = 0;
    char *out = xmalloc(cap);
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (n + 9 >= cap) { cap *= 2; out = xrealloc(out, cap); }
        if (*p == '\t') { size_t spaces = 8 - col % 8; memset(out + n, ' ', spaces); n += spaces; col += spaces; }
        else { out[n++] = (char)*p; if (*p == '\n') col = 0; else if ((*p & 0xC0) != 0x80) col++; }
    }
    out[n] = 0;
    return out;
}
static void paint_text(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT r = *rc;
    draw_text(cv, it->text, &r, it->font, it->color, it->flags);
}
/// Lays an item's text out in runs following its DT_ flags, so it paints and selects as one.
static void text_runs(Item *it, Canvas *cv) {
    UINT flags = it->flags;
    bool single = (flags & DT_SINGLELINE) != 0;
    int w = it->rc.right - it->rc.left, h = it->rc.bottom - it->rc.top;
    int align = (flags & DT_CENTER) ? ALIGN_CENTER : (flags & DT_RIGHT) ? ALIGN_RIGHT : ALIGN_LEFT;
    char *text = (flags & DT_EXPANDTABS) ? expand_tabs(it->text) : NULL;
    if (single) { char *one = str_replace(text ? text : it->text, "\n", " "); free(text); text = one; }
    Rich *r = rich_layout(cv, text ? text : it->text, single ? 100000 : w, it->font, it->color, true, 0, align, w);
    free(text);
    r->single = single;
    if (single) {
        int dy = (flags & DT_VCENTER) ? (h - r->height) / 2 : (flags & DT_BOTTOM) ? h - r->height : 0;
        if (dy) rich_offset(r, 0, dy);
    }
    it->data = r; it->free_data = rich_free; it->sel = r; it->paint = paint_rich;
}
int doc_text_at(Doc *doc, const RECT *rc, const char *text, FontId f, COLORREF color, UINT flags) {
    int i = doc_add(doc, rc, paint_text);
    Item *it = &doc->items[i];
    it->text = xstrdup(text ? text : ""); it->font = f; it->color = color; it->flags = flags;
    if (uses_runs(flags)) text_runs(it, doc->cv);
    return i;
}
int doc_text(Doc *doc, int x, int w, const char *text, FontId f, COLORREF color, UINT flags) {
    bool single = (flags & DT_SINGLELINE) != 0;
    RECT rc = { x, doc->y, x + w, doc->y + (single ? font_height(doc->cv, f) : 0) };
    int i = doc_text_at(doc, &rc, text, f, color, flags | (single ? 0 : DT_WORDBREAK | DT_EDITCONTROL));
    Item *it = &doc->items[i];
    if (!single) it->rc.bottom = it->rc.top + (it->sel ? it->sel->height : measure_text(doc->cv, text, w, f, flags & ~DT_VCENTER));
    doc->y = it->rc.bottom;
    return i;
}

// MARK: - Boxes

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    COLORREF fill = it->fill, border = it->border;
    bool hovered = it->action && it->hover_fill && doc_item_hovered(doc, it);
    if (hovered) { if (it->border != it->fill) border = theme.accent_dim; else fill = theme.raise; }
    if (it->action && doc->pressed >= 0 && &doc->items[doc->pressed] == it) fill = blend(theme.ink, fill, 0.06);
    fill_round_rect(cv, rc, it->radius, fill, border);
}
int doc_box_begin(Doc *doc, int x, int w, int pad, COLORREF fill, COLORREF border, int radius) {
    RECT rc = { x, doc->y, x + w, doc->y + pad };
    int i = doc_add(doc, &rc, paint_box);
    Item *it = &doc->items[i];
    it->fill = fill; it->border = border; it->radius = radius; it->hover_fill = true;
    doc->y += pad;
    return i;
}
void doc_box_end(Doc *doc, int box, int pad) {
    doc->y += pad;
    doc->items[box].rc.bottom = doc->y;
}
void doc_box_action(Doc *doc, int box, int action, intptr_t arg) {
    Item *it = &doc->items[box];
    it->action = action; it->arg = arg; it->hand = true;
}
void doc_space(Doc *doc, int h) { doc->y += h; }

static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { draw_line(cv, rc->left, rc->top, rc->right, rc->top, it->color); }
int doc_rule(Doc *doc, int x, int w) {
    RECT rc = { x, doc->y, x + w, doc->y + 1 };
    int i = doc_add(doc, &rc, paint_rule);
    doc->items[i].color = theme.border;
    doc->y += 1;
    return i;
}
int doc_custom(Doc *doc, int x, int w, int h, ItemPaint paint, void *data, ItemFree free_data, int action, intptr_t arg) {
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &rc, paint);
    Item *it = &doc->items[i];
    it->data = data; it->free_data = free_data; it->action = action; it->arg = arg; it->hand = action != 0;
    doc->y += h;
    return i;
}

// MARK: - Labels and notices

typedef struct { wchar_t glyph; FontId glyph_font; int glyph_w; } LabelData;
static void paint_label(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    LabelData *d = it->data;
    RECT g = { rc->left, rc->top, rc->left + d->glyph_w, rc->top + font_height(cv, it->font) + px(2) };
    draw_glyph(cv, d->glyph, &g, d->glyph_font, it->color);
    rich_paint(doc, it, it->sel, cv, rc);
}
static void paint_help(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    draw_glyph(cv, 0xE9CE, rc, FONT_ICON_SMALL, doc_item_hovered(doc, it) ? theme.ink : theme.muted);
}
int doc_field_label(Doc *doc, int x, int w, const char *label, COLORREF color, const char *help) {
    UINT flags = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS;
    if (!help || !*help) return doc_text(doc, x, w, label, FONT_FOOTNOTE, color, flags);
    int size = px(18), gap = px(4), top = doc->y;
    int lw = text_width(doc->cv, label, FONT_FOOTNOTE) + 2;
    if (lw > w - size - gap) lw = w - size - gap;
    if (lw < 0) lw = 0;
    int i = doc_text(doc, x, lw, label, FONT_FOOTNOTE, color, flags);
    int mid = (top + doc->y) / 2;
    RECT r = { x + lw + gap, mid - size / 2, x + lw + gap + size, mid - size / 2 + size };
    Item *it = doc_item(doc, doc_add(doc, &r, paint_help));
    it->action = ACTION_TIP;
    it->tip = str_replace(help, "`", "");   // the hints' inline code reads as plain text in a tooltip
    return i;
}
int doc_label(Doc *doc, int x, int w, wchar_t glyph, const char *text, FontId f, COLORREF color) {
    LabelData *d = xcalloc(1, sizeof *d);
    d->glyph = glyph; d->glyph_font = FONT_ICON_SMALL; d->glyph_w = px(16);
    Rich *r = rich_layout(doc->cv, text ? text : "", w - d->glyph_w - px(6), f, color, true, 0, ALIGN_LEFT, 0);
    rich_offset(r, d->glyph_w + px(6), 0);
    int h = r->height;
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &rc, paint_label);
    Item *it = &doc->items[i];
    it->data = d; it->free_data = free; it->sel = r; it->sel_owned = true;
    it->text = xstrdup(text ? text : ""); it->font = f; it->color = color; it->flags = DT_WORDBREAK | DT_EDITCONTROL;
    doc->y += h;
    return i;
}
int doc_notice(Doc *doc, int x, int w, const char *message) { return doc_text(doc, x, w, message, FONT_FOOTNOTE, theme.danger, DT_LEFT | DT_WORDBREAK); }
int doc_notice_box(Doc *doc, int x, int w, const char *message) {
    COLORREF fill = blend(theme.danger, theme.canvas, 0.15);
    int box = doc_box_begin(doc, x, w, px(10), fill, theme.danger, px(8));
    doc->items[box].hover_fill = false;
    doc_notice(doc, x + px(12), w - px(24), message);
    doc_box_end(doc, box, px(12));
    return box;
}

// MARK: - Buttons

typedef struct { ButtonStyle style; bool enabled, compact; wchar_t glyph; } ButtonData;
static void paint_button(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ButtonData *d = it->data;
    bool hovered = doc_item_hovered(doc, it) && d->enabled;
    bool pressed = doc->pressed >= 0 && &doc->items[doc->pressed] == it && d->enabled;
    COLORREF fill, border, text;
    switch (d->style) {
    case BUTTON_PROMINENT:
        fill = border = hovered ? theme.accent_dim : theme.accent; text = theme.on_accent;
        if (!d->enabled) { fill = border = blend(theme.accent, theme.raise, 0.5); }
        break;
    case BUTTON_DESTRUCTIVE:
        fill = theme.raise; border = hovered ? theme.danger : theme.line; text = hovered ? theme.danger : theme.ink; break;
    case BUTTON_PLAIN:
        fill = border = it->fill ? it->fill : theme.canvas; text = theme.accent; break;
    default:
        fill = theme.raise; border = hovered ? theme.accent_dim : theme.line; text = theme.ink; break;
    }
    if (pressed) fill = blend(theme.ink, fill, 0.08);
    if (!d->enabled && d->style != BUTTON_PROMINENT) text = theme.muted;
    if (d->style == BUTTON_PLAIN) {
        RECT t = *rc;
        draw_text(cv, it->text, &t, FONT_CAPTION, text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (hovered) { int tw = text_width(cv, it->text, FONT_CAPTION); draw_line(cv, rc->left, rc->bottom - px(3), rc->left + tw, rc->bottom - px(3), text); }
        return;
    }
    fill_round_rect(cv, rc, px(7), fill, border);
    RECT t = { rc->left + px(10), rc->top, rc->right - px(10) + 2, rc->bottom };
    draw_text(cv, it->text, &t, FONT_FOOTNOTE, text, (d->compact ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static int button_height(Canvas *cv) { return font_height(cv, FONT_FOOTNOTE) + px(12); }   // 18px line + 5px padding + 1px border, each side
static int button_width(Canvas *cv, const char *text) { return px(10) * 2 + text_width(cv, text, FONT_FOOTNOTE) + 2; }
int doc_button(Doc *doc, int x, int w, const char *text, ButtonStyle style, int action, intptr_t arg, bool enabled) {
    int h = style == BUTTON_PLAIN ? font_height(doc->cv, FONT_CAPTION) + px(6) : button_height(doc->cv);
    int tw = style == BUTTON_PLAIN ? text_width(doc->cv, text, FONT_CAPTION) + px(2) : button_width(doc->cv, text);
    int width = w > 0 ? w : w < 0 ? doc->width - x : tw;
    RECT rc = { x, doc->y, x + width, doc->y + h };
    int i = doc_add(doc, &rc, paint_button);
    Item *it = &doc->items[i];
    ButtonData *d = xcalloc(1, sizeof *d); d->style = style; d->enabled = enabled; d->compact = w <= 0;
    it->data = d; it->free_data = free; it->text = xstrdup(text);
    if (enabled) { it->action = action; it->arg = arg; it->hand = true; }
    doc->y += h;
    return i;
}

int doc_button_row(Doc *doc, int x, int w, const ButtonSpec *buttons, size_t count) {
    // `flex flex-wrap items-center gap-1.5`: the buttons wrap, 6px apart.
    int gap = px(6), cx = 0, cy = 0, first = -1;
    for (size_t k = 0; k < count; k++) {
        const ButtonSpec *b = &buttons[k];
        int h = b->style == BUTTON_PLAIN ? font_height(doc->cv, FONT_CAPTION) + px(6) : button_height(doc->cv);
        int bw = b->style == BUTTON_PLAIN ? text_width(doc->cv, b->text, FONT_CAPTION) + px(2) : button_width(doc->cv, b->text);
        if (bw > w) bw = w;
        if (cx > 0 && cx + bw > w) { cx = 0; cy += button_height(doc->cv) + gap; }
        int row_h = button_height(doc->cv);
        RECT rc = { x + cx, doc->y + cy + (row_h - h) / 2, x + cx + bw, doc->y + cy + (row_h - h) / 2 + h };
        int i = doc_add(doc, &rc, paint_button);
        Item *it = &doc->items[i];
        ButtonData *d = xcalloc(1, sizeof *d); d->style = b->style; d->enabled = b->enabled; d->compact = true; d->glyph = b->glyph;
        it->data = d; it->free_data = free; it->text = xstrdup(b->text);
        if (b->enabled) { it->action = b->action; it->arg = b->arg; it->hand = true; }
        if (first < 0) first = i;
        cx += bw + gap;
    }
    if (count) doc->y += cy + button_height(doc->cv);
    return first;
}

typedef struct { char **titles; size_t count; int selected; bool enabled; int action; intptr_t arg_base; } SegmentData;
static void segments_free(void *p) { SegmentData *d = p; str_array_free(d->titles, d->count); free(d); }
static int segment_width(Canvas *cv, const char *title) { return px(6) * 2 + text_width(cv, title, FONT_CAPTION2) + 2; }
static void paint_segments(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SegmentData *d = it->data;
    int x = rc->left;
    for (size_t i = 0; i < d->count; i++) {
        int w = segment_width(cv, d->titles[i]);
        RECT seg = { x, rc->top, x + w, rc->bottom };
        bool selected = (int)i == d->selected;
        COLORREF c = selected ? (i == 0 ? theme.danger : theme.accent) : theme.muted;
        if (!d->enabled) c = blend(c, theme.raise, 0.5);
        fill_round_rect(cv, &seg, px(4), it->fill ? it->fill : theme.raise, selected ? c : theme.line);
        draw_text(cv, d->titles[i], &seg, FONT_CAPTION2, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x += w + px(4);
    }
}
int doc_segments(Doc *doc, int x, int w, const char *const *titles, size_t count, int selected, int action, intptr_t arg_base, bool enabled) {
    int h = font_height(doc->cv, FONT_CAPTION2) + px(4);
    int total = 0;
    for (size_t k = 0; k < count; k++) total += segment_width(doc->cv, titles[k]) + (k ? px(4) : 0);
    RECT rc = { x, doc->y, x + (total < w ? total : w), doc->y + h };
    int i = doc_add(doc, &rc, paint_segments);
    Item *it = &doc->items[i];
    SegmentData *d = xcalloc(1, sizeof *d);
    d->titles = xmalloc((count ? count : 1) * sizeof *d->titles);
    for (size_t k = 0; k < count; k++) d->titles[k] = xstrdup(titles[k]);
    d->count = count; d->selected = selected; d->enabled = enabled; d->action = action; d->arg_base = arg_base;
    it->data = d; it->free_data = segments_free; it->fill = theme.raise;
    if (enabled) { it->action = action; it->arg = arg_base; it->hand = true; }
    doc->y += h;
    return i;
}
/// Segment index at a point; used by the pane to resolve the argument.
static int segment_at(Item *it, int x) {
    SegmentData *d = it->data;
    if (!d->count) return 0;
    Canvas *cv = NULL;   // measuring only
    int cx = it->rc.left, idx = (int)d->count - 1;
    for (size_t i = 0; i < d->count; i++) {
        int w = segment_width(cv, d->titles[i]);
        if (x < cx + w + px(2)) { idx = (int)i; break; }
        cx += w + px(4);
    }
    return idx;
}

int doc_loading(Doc *doc, int x, int w, const char *text) {
    doc_space(doc, px(8));
    int i = doc_text(doc, x + px(8), w - px(16), text ? text : "Loading\xE2\x80\xA6", FONT_FOOTNOTE, theme.muted, DT_LEFT);
    doc_space(doc, px(8));
    return i;
}

int doc_empty_state(Doc *doc, int x, int w, wchar_t glyph, const char *title, const char *detail) {
    (void)glyph;
    doc_space(doc, px(8));
    int first = doc_text(doc, x + px(8), w - px(16), title, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    if (detail) { doc_space(doc, px(2)); doc_text(doc, x + px(8), w - px(16), detail, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); }
    doc_space(doc, px(8));
    return first;
}

typedef struct { char *value; COLORREF value_color; int label_w; } LabeledData;
static void labeled_free(void *p) { LabeledData *d = p; free(d->value); free(d); }
static void paint_labeled(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    LabeledData *d = it->data;
    RECT l = { rc->left, rc->top, rc->left + d->label_w, rc->bottom };
    draw_text(cv, it->text, &l, FONT_CALLOUT, theme.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT v = { rc->left + d->label_w + px(12), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->value, &v, FONT_CALLOUT, d->value_color, DT_RIGHT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
}
int doc_labeled(Doc *doc, int x, int w, const char *label, const char *value, COLORREF value_color) {
    int label_w = text_width(doc->cv, label, FONT_CALLOUT);
    if (label_w > w / 2) label_w = w / 2;
    int h = measure_text(doc->cv, value, w - label_w - px(12), FONT_CALLOUT, DT_WORDBREAK);
    int lh = font_height(doc->cv, FONT_CALLOUT);
    if (h < lh) h = lh;
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &rc, paint_labeled);
    Item *it = &doc->items[i];
    LabeledData *d = xcalloc(1, sizeof *d); d->value = xstrdup(value ? value : ""); d->value_color = value_color; d->label_w = label_w;
    it->data = d; it->free_data = labeled_free; it->text = xstrdup(label);
    doc->y += h;
    return i;
}

typedef struct { BadgeSpec *specs; char **texts; RECT *rects; size_t count; COLORREF background; } BadgesData;
static void badges_free(void *p) { BadgesData *d = p; str_array_free(d->texts, d->count); free(d->specs); free(d->rects); free(d); }
static int badge_width(Canvas *cv, const BadgeSpec *b, const char *text, int *h) {
    // Measured by the painters themselves so the layout matches what is drawn.
    (void)cv;
    return b->chip ? draw_chip(NULL, 0, 0, text, b->color, 0, h) : draw_badge(NULL, 0, 0, b->glyph, text, b->color, 0, h);
}
static void paint_badges(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    BadgesData *d = it->data;
    for (size_t i = 0; i < d->count; i++) {
        int x = rc->left + d->rects[i].left, y = rc->top + d->rects[i].top;
        if (d->specs[i].chip) draw_chip(cv, x, y, d->texts[i], d->specs[i].color, d->background, NULL);
        else draw_badge(cv, x, y, d->specs[i].glyph, d->texts[i], d->specs[i].color, d->background, NULL);
    }
}
int doc_badges(Doc *doc, int x, int w, const BadgeSpec *badges, size_t count, COLORREF background) {
    BadgesData *d = xcalloc(1, sizeof *d);
    d->specs = xmalloc((count ? count : 1) * sizeof *d->specs); d->texts = xmalloc((count ? count : 1) * sizeof *d->texts);
    d->rects = xmalloc((count ? count : 1) * sizeof *d->rects); d->count = count; d->background = background;
    int cx = 0, cy = 0, row_h = 0, gap = px(5);
    for (size_t i = 0; i < count; i++) {
        d->specs[i] = badges[i]; d->texts[i] = xstrdup(badges[i].text ? badges[i].text : "");
        int h, bw = badge_width(doc->cv, &badges[i], d->texts[i], &h);
        if (bw > w) bw = w;
        if (cx > 0 && cx + bw > w) { cx = 0; cy += row_h + gap; row_h = 0; }
        RECT r = { cx, cy, cx + bw, cy + h }; d->rects[i] = r;
        cx += bw + gap; if (h > row_h) row_h = h;
    }
    int total = count ? cy + row_h : 0;
    RECT rc = { x, doc->y, x + w, doc->y + total };
    int i = doc_add(doc, &rc, paint_badges);
    Item *it = &doc->items[i];
    it->data = d; it->free_data = badges_free;
    doc->y += total;
    return i;
}

int doc_badges_width(Doc *doc, const BadgeSpec *badges, size_t count) {
    int total = 0;
    for (size_t i = 0; i < count; i++) { int h; total += badge_width(doc->cv, &badges[i], badges[i].text ? badges[i].text : "", &h) + (i ? px(5) : 0); }
    return total;
}

int doc_section(Doc *doc, int x, int w, const char *title) {
    doc_space(doc, px(12));
    int i = doc_text(doc, x, w, title, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(4));
    return i;
}

// MARK: - Markdown

typedef struct { char *language; char *code; bool copied; } CodeData;
static void code_free(void *p) { CodeData *d = p; free(d->language); free(d->code); free(d); }
static void doc_table(Doc *doc, int x, int w, const MdBlock *b, FontId base);
static void paint_task_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_code_header(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    CodeData *d = it->data;
    RECT l = { rc->left + px(12), rc->top, rc->right - px(40), rc->bottom };
    draw_text(cv, d->language ? d->language : "code", &l, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT g = { rc->right - px(34), rc->top, rc->right - px(6), rc->bottom };
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, &g, px(6), blend(theme.text, theme.code, 0.06), blend(theme.text, theme.code, 0.06));
    draw_glyph(cv, d->copied ? 0xE73E : 0xE8C8, &g, FONT_ICON_SMALL, theme.secondary);
    draw_line(cv, rc->left, rc->bottom - 1, rc->right, rc->bottom - 1, theme.border);
}
static void quote_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { RECT r = { rc->left, rc->top, rc->left + px(2), rc->bottom }; fill_rect(cv, &r, theme.line); }
static void paint_task_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool checked = it->arg != 0;
    fill_round_rect(cv, rc, px(3), checked ? theme.accent : theme.elevated, checked ? theme.accent : blend(theme.text, theme.background, 0.35));
    if (checked) draw_check_mark(cv, rc, theme.white);
}

// MARK: - Tables

// The table item paints the grid; each cell is a text item over it, so it selects and copies like any other text.
typedef struct { size_t rows, cols; int *col_x, *row_y, *row_h; } TableData;
static void table_free(void *p) {
    TableData *t = p;
    free(t->col_x); free(t->row_y); free(t->row_h); free(t);
}
static void paint_table(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TableData *t = it->data;
    // Header tint, zebra rows and a grid.
    for (size_t r = 0; r < t->rows; r++) {
        RECT row = { rc->left, rc->top + t->row_y[r], rc->right, rc->top + t->row_y[r] + t->row_h[r] };
        if (r == 0) fill_rect(cv, &row, theme.raise);
        draw_line(cv, rc->left, row.bottom, rc->right, row.bottom, theme.line);
    }
    draw_line(cv, rc->left, rc->top, rc->right, rc->top, theme.border);
    for (size_t c = 0; c <= t->cols; c++) {
        int x = c < t->cols ? rc->left + t->col_x[c] : rc->right - 1;
        draw_line(cv, x, rc->top, x, rc->bottom, theme.border);
    }
}
static void paint_table_copy(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    RECT g = { rc->left + px(4), rc->top, rc->left + px(22), rc->bottom };
    draw_glyph(cv, 0xE8C8, &g, FONT_ICON_SMALL, theme.secondary);
    RECT l = { g.right + px(2), rc->top, rc->right - px(6), rc->bottom };
    draw_text(cv, "Copy table", &l, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}
/// A table sized to its content: columns take what their widest cell wants, then share the width that is left. A copy
/// button above its right edge copies it as Markdown.
static void doc_table(Doc *doc, int x, int w, const MdBlock *b, FontId base) {
    TableData *t = xcalloc(1, sizeof *t);
    t->rows = b->rows; t->cols = b->cols;
    Rich **cells = xcalloc(t->rows * t->cols, sizeof *cells);
    int *col_w = xcalloc(t->cols, sizeof *col_w);
    t->col_x = xcalloc(t->cols, sizeof *t->col_x);
    t->row_y = xcalloc(t->rows, sizeof *t->row_y); t->row_h = xcalloc(t->rows, sizeof *t->row_h);
    int pad = px(8);
    FontId cell_font = base == FONT_BODY ? FONT_CALLOUT : base;
    // Natural widths, measured unwrapped.
    int *want = xcalloc(t->cols, sizeof *want);
    for (size_t r = 0; r < t->rows; r++)
        for (size_t c = 0; c < t->cols; c++) {
            Rich *probe = rich_layout(doc->cv, b->cells[r * t->cols + c], 100000, r == 0 ? rich_font(cell_font, SPAN_BOLD) : cell_font, theme.text, false, px(3), ALIGN_LEFT, 0);
            int widest = 0;
            for (size_t k = 0; k < probe->count; k++) if (probe->runs[k].x + probe->runs[k].w > widest) widest = probe->runs[k].x + probe->runs[k].w;
            rich_free(probe);
            if (widest + 2 * pad > want[c]) want[c] = widest + 2 * pad;
        }
    int total = 0, min_w = px(48);
    for (size_t c = 0; c < t->cols; c++) { if (want[c] < min_w) want[c] = min_w; total += want[c]; }
    if (total > w) {
        // Shrink the widest columns first; every column keeps at least a minimum.
        int flexible = 0;
        for (size_t c = 0; c < t->cols; c++) if (want[c] > min_w) flexible += want[c] - min_w;
        int excess = total - w;
        for (size_t c = 0; c < t->cols; c++) {
            if (want[c] <= min_w || flexible <= 0) continue;
            int cut = (int)((long long)excess * (want[c] - min_w) / flexible);
            want[c] -= cut;
        }
        total = 0; for (size_t c = 0; c < t->cols; c++) total += want[c];
    }
    int cx = 0;
    for (size_t c = 0; c < t->cols; c++) { t->col_x[c] = cx; col_w[c] = want[c]; cx += want[c]; }
    int table_w = cx < w ? cx : w;
    int y = 1;
    for (size_t r = 0; r < t->rows; r++) {
        int h = 0;
        for (size_t c = 0; c < t->cols; c++) {
            int cell_w = col_w[c] - 2 * pad; if (cell_w < px(16)) cell_w = px(16);
            Rich *cell = rich_layout(doc->cv, b->cells[r * t->cols + c], cell_w, r == 0 ? rich_font(cell_font, SPAN_BOLD) : cell_font, theme.text, false, px(3), ALIGN_LEFT, 0);
            cells[r * t->cols + c] = cell;
            if (cell->height > h) h = cell->height;
        }
        t->row_y[r] = y; t->row_h[r] = h + px(10);
        y += t->row_h[r] + 1;
    }
    free(want);
    int copy_w = text_width(doc->cv, "Copy table", FONT_CAPTION) + px(30), copy_h = font_height(doc->cv, FONT_CAPTION) + px(8);
    // A table narrower than the button lets it run past its right edge, so the label is not clipped.
    int copy_right = table_w < copy_w ? copy_w : table_w;
    if (copy_right > w) copy_right = w;
    RECT cr = { x + copy_right - copy_w, doc->y, x + copy_right, doc->y + copy_h };
    if (cr.left < x) cr.left = x;
    char *source = md_table_source(b);
    Item *ci = &doc->items[doc_add(doc, &cr, paint_table_copy)];
    ci->data = source; ci->free_data = free; ci->action = ACTION_COPY_CODE; ci->arg = (intptr_t)source; ci->hand = true;
    doc->y += copy_h + px(2);
    RECT rc = { x, doc->y, x + table_w, doc->y + y };
    int i = doc_add(doc, &rc, paint_table);
    Item *it = &doc->items[i];
    it->data = t; it->free_data = table_free;
    for (size_t r = 0; r < t->rows; r++) {
        for (size_t c = 0; c < t->cols; c++) {
            Rich *cell = cells[r * t->cols + c];
            int cell_w = col_w[c] - 2 * pad;
            int shift = 0;
            if (b->aligns[c] != 'l') {
                int widest = 0;
                for (size_t k = 0; k < cell->count; k++) if (cell->runs[k].x + cell->runs[k].w > widest) widest = cell->runs[k].x + cell->runs[k].w;
                shift = b->aligns[c] == 'r' ? cell_w - widest : (cell_w - widest) / 2;
                if (shift < 0) shift = 0;
            }
            // The item covers the whole row, its runs moved below the top padding, so a press there lands in this cell.
            rich_offset(cell, 0, px(5));
            RECT cell_rc = { rc.left + t->col_x[c] + pad + shift, rc.top + t->row_y[r], rc.left + t->col_x[c] + col_w[c] - pad, rc.top + t->row_y[r] + t->row_h[r] };
            Item *cell_it = &doc->items[doc_add(doc, &cell_rc, paint_rich)];
            cell_it->data = cell; cell_it->free_data = rich_free; cell_it->sel = cell; cell_it->cell = true;
            for (size_t k = 0; k < cell->count; k++) if (cell->runs[k].link) { cell_it->action = ACTION_OPEN_LINK; cell_it->hand = true; break; }
        }
    }
    free(cells); free(col_w);
    doc->y += y;
}

void doc_markdown(Doc *doc, int x, int w, const char *source, FontId base) {
    size_t n; MdBlock *blocks = md_parse(source, &n);
    int gap = px(6);
    for (size_t i = 0; i < n; i++) {
        if (i) doc_space(doc, gap);
        MdBlock *b = &blocks[i];
        switch (b->kind) {
        case MD_PARAGRAPH: doc_rich(doc, x, w, b->text, base, theme.text); break;
        case MD_HEADING:
            doc_space(doc, px(6));
            doc_rich(doc, x, w, b->text, FONT_TITLE3, theme.text);
            break;
        case MD_BULLET: {
            int indent = b->indent * px(16);
            int top = doc->y;
            int marker_w;
            if (b->task) {
                // A task list item shows its box instead of a bullet.
                marker_w = px(22);
                RECT br = { x + indent + px(1), top + px(2), x + indent + px(15), top + px(16) };
                int bi = doc_add(doc, &br, paint_task_box);
                doc_item(doc, bi)->arg = b->task == 2;
            } else {
                marker_w = text_width(doc->cv, b->marker, base) + px(8);
                if (marker_w < px(18)) marker_w = px(18);
                RECT mr = { x + indent, top, x + indent + marker_w, top + font_height(doc->cv, base) + px(3) };
                doc_text_at(doc, &mr, b->marker, base, theme.secondary, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            }
            doc_rich(doc, x + indent + marker_w, w - indent - marker_w, b->text, base, b->task == 2 ? theme.secondary : theme.text);
            break;
        }
        case MD_QUOTE: {
            int top = doc->y;
            doc_rich(doc, x + px(12), w - px(12), b->text, base, theme.muted);
            RECT bar = { x, top, x + px(2), doc->y };
            doc_add(doc, &bar, quote_bar);
            break;
        }
        case MD_CODE: {
            int box = doc_box_begin(doc, x, w, 0, theme.sunken, theme.line, px(8));
            doc->items[box].hover_fill = false;
            int header_h = font_height(doc->cv, FONT_MONO_CAPTION2) + px(12);
            RECT hr = { x, doc->y, x + w, doc->y + header_h };
            int header = doc_add(doc, &hr, paint_code_header);
            CodeData *d = xcalloc(1, sizeof *d); d->language = xstrdup(b->language); d->code = xstrdup(b->text);
            Item *hi = &doc->items[header];
            hi->data = d; hi->free_data = code_free; hi->action = ACTION_COPY_CODE; hi->arg = (intptr_t)d->code; hi->hand = true;
            doc->y += header_h;
            doc_space(doc, px(6));
            doc_text(doc, x + px(10), w - px(20), b->text, FONT_MONO, theme.text, DT_WORDBREAK | DT_EXPANDTABS);
            doc_box_end(doc, box, px(10));
            break;
        }
        case MD_RULE: doc_space(doc, px(4)); doc_rule(doc, x, w); doc_space(doc, px(4)); break;
        case MD_TABLE: doc_table(doc, x, w, b, base); break;
        }
    }
    md_free(blocks, n);
}

// MARK: - Sticky items

void doc_sticky(Doc *doc, int first, int last, int limit) {
    if (first < 0) first = 0;
    if (last > (int)doc->count) last = (int)doc->count;
    doc->sticky_first = first; doc->sticky_last = last > first ? last : first;
    doc->sticky_limit = limit; doc->sticky_shift = 0;
}
static bool sticky_active(const Doc *doc) { return doc->sticky_first < doc->sticky_last; }
void doc_pin(Doc *doc, int last, int bottom) {
    if (last > (int)doc->count) last = (int)doc->count;
    doc->pin_last = last > 0 ? last : 0; doc->pin_bottom = bottom; doc->pin_shift = 0;
}
static bool pin_active(const Doc *doc) { return doc->pin_last > 0; }
/// Whether item `i` is out of reach at content y: the pinned band covers the items scrolled beneath it, and the
/// pinned items are only under the mouse within the band.
static bool pin_covered(const Doc *doc, size_t i, int y) {
    return pin_active(doc) && ((int)i < doc->pin_last) != (y < doc->pin_shift + doc->pin_bottom);
}
static void sticky_move(Doc *doc, int shift) {
    int delta = shift - doc->sticky_shift;
    if (!delta) return;
    for (int i = doc->sticky_first; i < doc->sticky_last; i++) OffsetRect(&doc->items[i].rc, 0, delta);
    doc->sticky_shift = shift;
}
void doc_set_view(Doc *doc, int scroll_y, int view_height) {
    if (pin_active(doc) && scroll_y != doc->pin_shift) {
        for (int i = 0; i < doc->pin_last; i++) OffsetRect(&doc->items[i].rc, 0, scroll_y - doc->pin_shift);
        doc->pin_shift = scroll_y;
    }
    if (!sticky_active(doc)) { doc->sticky_scroll = doc->sticky_max = 0; return; }
    // The group's bounds where it was laid out.
    int top = INT_MAX, bottom = INT_MIN, left = INT_MAX, right = INT_MIN;
    for (int i = doc->sticky_first; i < doc->sticky_last; i++) {
        const RECT *rc = &doc->items[i].rc;
        if (rc->top < top) top = rc->top;
        if (rc->bottom > bottom) bottom = rc->bottom;
        if (rc->left < left) left = rc->left;
        if (rc->right > right) right = rc->right;
    }
    top -= doc->sticky_shift; bottom -= doc->sticky_shift;
    // The window keeps a 12px margin in the view and stops at the limit; the group scrolls inside it. Under a pinned
    // band it stays where it was laid out, below the band.
    int pad = px(12), height = bottom - top;
    int head = pin_active(doc) ? (top > doc->pin_bottom ? top : doc->pin_bottom) : pad;
    int want = view_height - head - pad; if (want > height) want = height; if (want < 0) want = 0;
    int win_top = scroll_y + head;
    if (win_top > doc->sticky_limit - want) win_top = doc->sticky_limit - want;
    if (win_top < top) win_top = top;
    int win_bottom = scroll_y + view_height - pad;
    if (win_bottom > win_top + height) win_bottom = win_top + height;
    if (win_bottom > doc->sticky_limit) win_bottom = doc->sticky_limit;
    if (win_bottom < win_top) win_bottom = win_top;
    doc->sticky_max = height - (win_bottom - win_top);
    if (doc->sticky_scroll > doc->sticky_max) doc->sticky_scroll = doc->sticky_max;
    if (doc->sticky_scroll < 0) doc->sticky_scroll = 0;
    SetRect(&doc->sticky_view, left, win_top, right, win_bottom);
    sticky_move(doc, win_top - top - doc->sticky_scroll);
}
bool doc_sticky_wheel(Doc *doc, int x, int y, int dy) {
    if (!sticky_active(doc) || doc->sticky_max <= 0) return false;
    const RECT *v = &doc->sticky_view;
    if (x < v->left || x >= v->right || y < v->top || y >= v->bottom) return false;
    int s = doc->sticky_scroll + dy;
    if (s > doc->sticky_max) s = doc->sticky_max;
    if (s < 0) s = 0;
    sticky_move(doc, doc->sticky_shift - (s - doc->sticky_scroll));
    doc->sticky_scroll = s;
    return true;
}
/// A sticky item scrolled out of the group's window at this content y.
static bool sticky_hidden(const Doc *doc, size_t i, int y) {
    return sticky_active(doc) && (int)i >= doc->sticky_first && (int)i < doc->sticky_last && (y < doc->sticky_view.top || y >= doc->sticky_view.bottom);
}

// MARK: - Scrolling regions

static void region_scroll(Doc *doc, DocRegion *r, int scroll) {
    if (scroll > r->max) scroll = r->max;
    if (scroll < 0) scroll = 0;
    int delta = r->scroll - scroll;
    if (!delta) return;
    for (int i = r->first; i < r->last; i++) OffsetRect(&doc->items[i].rc, 0, delta);
    r->scroll = scroll;
}
void doc_region(Doc *doc, int first, int last, const RECT *view, int bottom) {
    if (first < 0) first = 0;
    if (last > (int)doc->count) last = (int)doc->count;
    if (doc->region_count == doc->region_cap) { doc->region_cap = doc->region_cap ? doc->region_cap * 2 : 16; doc->regions = xrealloc(doc->regions, doc->region_cap * sizeof *doc->regions); }
    size_t index = doc->region_count++;
    DocRegion *r = &doc->regions[index];
    int kept = index < doc->regions_kept ? r->scroll : 0;
    r->first = first; r->last = last > first ? last : first; r->view = *view; r->bottom = bottom;
    r->scroll = 0;
    r->max = bottom - view->bottom; if (r->max < 0) r->max = 0;
    region_scroll(doc, r, kept);
}
/// The region item `i` scrolls in, or NULL.
static const DocRegion *region_of(const Doc *doc, size_t i) {
    for (size_t k = 0; k < doc->region_count; k++) if ((int)i >= doc->regions[k].first && (int)i < doc->regions[k].last) return &doc->regions[k];
    return NULL;
}
static bool in_view(const RECT *v, int x, int y) { return x >= v->left && x < v->right && y >= v->top && y < v->bottom; }
/// A region's item out of reach at a content point: the region shows it only through its window.
static bool region_hidden(const Doc *doc, size_t i, int x, int y) {
    if (!doc->region_count) return false;
    const DocRegion *r = region_of(doc, i);
    return r && !in_view(&r->view, x, y);
}
static int region_at(const Doc *doc, int x, int y) {
    for (size_t k = 0; k < doc->region_count; k++) if (in_view(&doc->regions[k].view, x, y)) return (int)k;
    return -1;
}
bool doc_region_wheel(Doc *doc, int x, int y, int dy) {
    int k = region_at(doc, x, y);
    if (k < 0 || doc->regions[k].max <= 0) return false;
    region_scroll(doc, &doc->regions[k], doc->regions[k].scroll + dy);
    return true;
}
/// The bar's thumb, in content coordinates; false while the region does not overflow.
static bool region_thumb(const DocRegion *r, RECT *thumb) {
    if (r->max <= 0) return false;
    int track = r->view.bottom - r->view.top, total = track + r->max;
    int h = track * track / total; if (h < px(24)) h = px(24);
    int y = r->view.top + (track - h) * r->scroll / r->max;
    SetRect(thumb, r->view.right - px(7), y, r->view.right - px(1), y + h);
    return true;
}
int doc_region_thumb_at(Doc *doc, int x, int y, int *thumb_top) {
    for (size_t k = 0; k < doc->region_count; k++) {
        RECT t;
        if (!region_thumb(&doc->regions[k], &t)) continue;
        // A little wider than it is drawn, as it is thin.
        if (x >= t.left - px(4) && x < t.right + px(2) && y >= t.top && y < t.bottom) { *thumb_top = t.top; return (int)k; }
    }
    return -1;
}
void doc_region_drag(Doc *doc, int k, int thumb_top) {
    if (k < 0 || (size_t)k >= doc->region_count) return;
    DocRegion *r = &doc->regions[k];
    RECT t;
    if (!region_thumb(r, &t)) return;
    int room = (r->view.bottom - r->view.top) - (t.bottom - t.top);
    if (room > 0) region_scroll(doc, r, (thumb_top - r->view.top) * r->max / room);
}

// MARK: - Paint and hit

static void paint_item(Doc *doc, Item *it, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip) {
    RECT rc = { it->rc.left - scroll_x, it->rc.top - scroll_y, it->rc.right - scroll_x, it->rc.bottom - scroll_y };
    if (rc.bottom < clip->top - px(4) || rc.top > clip->bottom + px(4)) return;
    if (rc.right < clip->left || rc.left > clip->right) return;
    // A carried box leaves an empty slot behind.
    int i = (int)(it - doc->items);
    if (i >= doc->drag_first && i < doc->drag_last) { if (i == doc->drag_first) fill_round_rect(cv, &rc, it->radius, theme.sunken, theme.line); return; }
    if (it->paint) it->paint(doc, it, cv, &rc);
}

// MARK: - Carrying a box

void doc_drag(Doc *doc, int box) {
    doc->drag_first = doc->drag_last = 0;
    if (box < 0 || (size_t)box >= doc->count) return;
    const RECT *b = &doc->items[box].rc;
    int last = box + 1;
    while ((size_t)last < doc->count) {
        const RECT *r = &doc->items[last].rc;
        if (r->left < b->left || r->right > b->right || r->top < b->top || r->bottom > b->bottom) break;
        last++;
    }
    doc->drag_first = box; doc->drag_last = last;
}
void doc_paint_dragged(Doc *doc, Canvas *cv, int x, int y) {
    if (doc->drag_first >= doc->drag_last) return;
    Item *box = &doc->items[doc->drag_first];
    int dx = x - box->rc.left, dy = y - box->rc.top;
    // Lifted off the page: a shadow under it and the accent round it.
    RECT shadow = box->rc; OffsetRect(&shadow, dx + px(3), dy + px(4));
    COLORREF shade = blend(RGB(0, 0, 0), theme.canvas, 0.35);
    fill_round_rect(cv, &shadow, box->radius, shade, shade);
    for (int i = doc->drag_first; i < doc->drag_last; i++) {
        Item *it = &doc->items[i];
        RECT rc = it->rc; OffsetRect(&rc, dx, dy);
        if (i == doc->drag_first) { fill_round_rect(cv, &rc, it->radius, it->fill, theme.accent); continue; }
        if (it->paint) it->paint(doc, it, cv, &rc);
    }
}
/// The items in [first, last) that scroll with the page; the regions' own are painted through their windows.
static void paint_items(Doc *doc, Canvas *cv, int first, int last, int scroll_x, int scroll_y, const RECT *clip) {
    for (int i = first; i < last; i++) if (!doc->region_count || !region_of(doc, (size_t)i)) paint_item(doc, &doc->items[i], cv, scroll_x, scroll_y, clip);
}
static void paint_regions(Doc *doc, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip) {
    for (size_t k = 0; k < doc->region_count; k++) {
        const DocRegion *r = &doc->regions[k];
        RECT v = r->view; OffsetRect(&v, -scroll_x, -scroll_y);
        RECT within;
        if (!IntersectRect(&within, &v, clip)) continue;
        canvas_clip(cv, &within);
        for (int i = r->first; i < r->last; i++) paint_item(doc, &doc->items[i], cv, scroll_x, scroll_y, &within);
        canvas_unclip(cv);
        RECT t;
        if (region_thumb(r, &t)) { OffsetRect(&t, -scroll_x, -scroll_y); fill_round_rect(cv, &t, px(3), theme.thumb, theme.thumb); }
    }
}
static void paint_scrolled(Doc *doc, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip);
void doc_paint(Doc *doc, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip) {
    if (!pin_active(doc)) { paint_scrolled(doc, cv, scroll_x, scroll_y, clip); return; }
    // The scrolling items show below the band, and the pinned ones within it.
    RECT below = *clip; if (below.top < doc->pin_bottom) below.top = doc->pin_bottom;
    if (below.top < below.bottom) { canvas_clip(cv, &below); paint_scrolled(doc, cv, scroll_x, scroll_y, &below); canvas_unclip(cv); }
    RECT band = *clip; if (band.bottom > doc->pin_bottom) band.bottom = doc->pin_bottom;
    canvas_clip(cv, &band);
    paint_items(doc, cv, 0, doc->pin_last, scroll_x, scroll_y, &band);
    canvas_unclip(cv);
}
/// The items that scroll: all of them but the pinned band, the sticky group through its window.
static void paint_scrolled(Doc *doc, Canvas *cv, int scroll_x, int scroll_y, const RECT *clip) {
    int first = doc->pin_last;
    if (!sticky_active(doc)) { paint_items(doc, cv, first, (int)doc->count, scroll_x, scroll_y, clip); paint_regions(doc, cv, scroll_x, scroll_y, clip); return; }
    paint_items(doc, cv, first, doc->sticky_first, scroll_x, scroll_y, clip);
    // The sticky group shows through its window, with a thin bar at its right edge while it overflows.
    RECT v = doc->sticky_view; OffsetRect(&v, -scroll_x, -scroll_y);
    RECT within; IntersectRect(&within, &v, clip);
    canvas_clip(cv, &v);
    paint_items(doc, cv, doc->sticky_first, doc->sticky_last, scroll_x, scroll_y, &within);
    canvas_unclip(cv);
    if (doc->sticky_max > 0) {
        int track = v.bottom - v.top, total = track + doc->sticky_max;
        int thumb = track * track / total; if (thumb < px(24)) thumb = px(24);
        int y = v.top + (track - thumb) * doc->sticky_scroll / doc->sticky_max;
        RECT r = { v.right - px(6), y, v.right - px(1), y + thumb };
        COLORREF c = RGB(0x3C, 0x3B, 0x38);
        fill_round_rect(cv, &r, px(3), c, c);
    }
    paint_items(doc, cv, doc->sticky_last, (int)doc->count, scroll_x, scroll_y, clip);
    paint_regions(doc, cv, scroll_x, scroll_y, clip);
}
int doc_hit(Doc *doc, int x, int y) {
    for (size_t i = doc->count; i-- > 0;) {
        Item *it = &doc->items[i];
        if (!it->action || sticky_hidden(doc, i, y) || pin_covered(doc, i, y) || region_hidden(doc, i, x, y)) continue;
        if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) {
            if (it->paint == paint_segments) it->arg = ((SegmentData *)it->data)->arg_base + segment_at(it, x);
            // Linked text is clickable on its links alone; the rest of it selects.
            if (it->action == ACTION_OPEN_LINK && !doc_link_at(doc, (int)i, x, y)) continue;
            return (int)i;
        }
    }
    return -1;
}
char *doc_item_plain_text(Doc *doc, int index) {
    Item *it = doc_item(doc, index);
    if (!it) return NULL;
    if (it->sel) return wide_to_utf8(it->sel->plain);
    if (!it->text) return NULL;
    return it->paint == paint_rich ? md_plain(it->text) : xstrdup(it->text);
}
int doc_find(Doc *doc, int id) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].id == id) return (int)i;
    return -1;
}

// MARK: - Selection

static bool pos_before(DocPos a, DocPos b) { return a.item < b.item || (a.item == b.item && a.offset < b.offset); }
static void clamp_pos(Doc *doc, DocPos *p) {
    Rich *r = doc->items[p->item].sel;
    if (!r) p->offset = 0;
    else if (p->offset < 0) p->offset = 0;
    else if ((size_t)p->offset > r->plain_len) p->offset = (int)r->plain_len;
}
/// The selection in document order, clamped to the items laid out; false when it is empty.
static bool selection_range(Doc *doc, DocPos *a, DocPos *b) {
    DocPos s = doc->sel_anchor, e = doc->sel_focus;
    if (s.item < 0 || e.item < 0 || (size_t)s.item >= doc->count || (size_t)e.item >= doc->count) return false;
    if (pos_before(e, s)) { DocPos t = s; s = e; e = t; }
    clamp_pos(doc, &s); clamp_pos(doc, &e);
    *a = s; *b = e;
    return pos_before(s, e);
}
/// The part of an item's plain text that is selected.
static bool item_selection(Doc *doc, Item *it, size_t *from, size_t *to) {
    if (!it || !it->sel || it < doc->items || it >= doc->items + doc->count) return false;
    int i = (int)(it - doc->items);
    DocPos a, b;
    if (!selection_range(doc, &a, &b) || i < a.item || i > b.item) return false;
    *from = i == a.item ? (size_t)a.offset : 0;
    *to = i == b.item ? (size_t)b.offset : it->sel->plain_len;
    return *from < *to;
}
bool doc_has_selection(Doc *doc) { DocPos a, b; return selection_range(doc, &a, &b); }
void doc_clear_selection(Doc *doc) {
    doc->sel_anchor.item = doc->sel_focus.item = -1; doc->sel_anchor.offset = doc->sel_focus.offset = 0;
    doc->selecting = false;
}
int doc_text_item_at(Doc *doc, int x, int y) {
    for (size_t i = doc->count; i-- > 0;) {
        Item *it = &doc->items[i];
        if (it->sel && !pin_covered(doc, i, y) && !region_hidden(doc, i, x, y) && x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) return (int)i;
    }
    return -1;
}

/// The character boundary nearest `x` within a run's text; `x` is from the text's left edge.
static size_t run_char_at(Canvas *cv, const Run *run, int x) {
    if (x <= 0 || !run->len) return 0;
    size_t k = textw_fit(run->font, run->text, run->len, x);
    if (k < run->len) {
        int a = textw_extent(run->font, run->text, k), b = textw_extent(run->font, run->text, k + 1);
        if (x > (a + b) / 2) k++;
    }
    return k;
}
/// The plain-text offset nearest a point relative to the runs' origin.
static size_t rich_hit(Canvas *cv, const Rich *r, int x, int y) {
    if (!r->lines) return 0;
    if (y < r->line_y[0]) return 0;
    int last = r->lines - 1;
    if (y >= r->line_y[last] + r->line_h[last]) return r->plain_len;
    int l = last;
    for (int k = 0; k < r->lines; k++) if (y < r->line_y[k] + r->line_h[k]) { l = k; break; }
    const Run *first = NULL, *end = NULL;
    for (size_t i = 0; i < r->count; i++) {
        const Run *run = &r->runs[i];
        if (run->line != l) continue;
        if (!first) first = run;
        end = run;
        if (x < run->x + run->w) {
            if (x < run->x) return run->start;
            return run->start + run_char_at(cv, run, x - run->x - run->pad);
        }
    }
    if (!first) return r->line_start[l];   // a blank line
    return end->start + end->len;
}
bool doc_position_at(Doc *doc, Canvas *cv, int x, int y, DocPos *pos) {
    // The item spanning y and nearest x; failing that, the nearest item above (its end) or below (its start).
    int best = -1, best_d = 0;
    for (size_t i = 0; i < doc->count; i++) {
        Item *it = &doc->items[i];
        if (!it->sel || pin_covered(doc, i, y) || region_hidden(doc, i, x, y) || y < it->rc.top || y >= it->rc.bottom) continue;
        int d = x < it->rc.left ? it->rc.left - x : x >= it->rc.right ? x - it->rc.right + 1 : 0;
        if (best < 0 || d < best_d) { best = (int)i; best_d = d; }
    }
    if (best >= 0) {
        Item *it = &doc->items[best];
        pos->item = best; pos->offset = (int)rich_hit(cv, it->sel, x - it->rc.left, y - it->rc.top);
        return true;
    }
    for (size_t i = 0; i < doc->count; i++) {
        Item *it = &doc->items[i];
        if (!it->sel) continue;
        int d = y < it->rc.top ? it->rc.top - y : y - it->rc.bottom + 1;
        if (best < 0 || d < best_d) { best = (int)i; best_d = d; }
    }
    if (best < 0) return false;
    Item *it = &doc->items[best];
    pos->item = best; pos->offset = y < it->rc.top ? 0 : (int)it->sel->plain_len;
    return true;
}
void doc_select_all(Doc *doc) {
    int first = -1, last = -1;
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].sel) { if (first < 0) first = (int)i; last = (int)i; }
    if (first < 0) return;
    doc->sel_anchor.item = first; doc->sel_anchor.offset = 0;
    doc->sel_focus.item = last; doc->sel_focus.offset = (int)doc->items[last].sel->plain_len;
}
void doc_select_word(Doc *doc, DocPos pos) {
    Item *it = doc_item(doc, pos.item);
    if (!it || !it->sel || !it->sel->plain_len) return;
    const wchar_t *p = it->sel->plain; size_t n = it->sel->plain_len;
    size_t o = pos.offset < 0 ? 0 : (size_t)pos.offset >= n ? n - 1 : (size_t)pos.offset;
    bool space = iswspace(p[o]) != 0;
    size_t s = o, e = o + 1;
    while (s > 0 && (iswspace(p[s - 1]) != 0) == space && p[s - 1] != L'\n') s--;
    while (e < n && (iswspace(p[e]) != 0) == space && p[e] != L'\n') e++;
    doc->sel_anchor.item = doc->sel_focus.item = pos.item;
    doc->sel_anchor.offset = (int)s; doc->sel_focus.offset = (int)e;
}
char *doc_selection_text(Doc *doc) {
    DocPos a, b;
    if (!selection_range(doc, &a, &b)) return NULL;
    wchar_t *out = NULL; size_t len = 0, cap = 0;
    int prev_bottom = 0; bool any = false;
    for (int i = a.item; i <= b.item; i++) {
        Item *it = &doc->items[i];
        if (!it->sel) continue;
        size_t from = i == a.item ? (size_t)a.offset : 0, to = i == b.item ? (size_t)b.offset : it->sel->plain_len;
        // An empty table cell still takes its place, so the cells after it stay in their columns.
        if (from >= to && !(it->cell && !it->sel->plain_len)) continue;
        // Items side by side (a bullet and its text) join with a space, table cells with a tab; stacked ones take a line each.
        const wchar_t *sep = !any ? L"" : it->rc.top < prev_bottom - px(2) ? (it->cell ? L"\t" : L" ") : L"\n";
        size_t need = len + wcslen(sep) + (to - from) + 1;
        if (need > cap) { cap = need * 2; out = xrealloc(out, cap * sizeof *out); }
        wcscpy(out + len, sep); len += wcslen(sep);
        memcpy(out + len, it->sel->plain + from, (to - from) * sizeof *out); len += to - from; out[len] = 0;
        prev_bottom = it->rc.bottom; any = true;
    }
    if (!out) return NULL;
    char *utf8 = wide_to_utf8(out);
    free(out);
    return utf8;
}
