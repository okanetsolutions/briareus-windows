#include "doc.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

// MARK: - Items

void doc_init(Doc *doc) { memset(doc, 0, sizeof *doc); doc->hover = -1; doc->pressed = -1; }
static void clear_items(Doc *doc) {
    for (size_t i = 0; i < doc->count; i++) {
        Item *it = &doc->items[i];
        if (it->free_data) it->free_data(it->data);
        free(it->text);
    }
    doc->count = 0;
}
void doc_free(Doc *doc) { clear_items(doc); free(doc->items); doc_init(doc); }
void doc_begin(Doc *doc, HDC hdc, int width) {
    clear_items(doc);
    doc->hdc = hdc; doc->width = width; doc->y = 0; doc->content_width = width; doc->hover = -1; doc->pressed = -1;
}
void doc_end(Doc *doc) { doc->hdc = NULL; }
Item *doc_item(Doc *doc, int index) { return index >= 0 && (size_t)index < doc->count ? &doc->items[index] : NULL; }
int doc_height(const Doc *doc) { return doc->y; }

int doc_add(Doc *doc, const RECT *rc, ItemPaint paint) {
    if (doc->count == doc->cap) { doc->cap = doc->cap ? doc->cap * 2 : 64; doc->items = xrealloc(doc->items, doc->cap * sizeof *doc->items); }
    Item *it = &doc->items[doc->count];
    memset(it, 0, sizeof *it);
    it->rc = *rc; it->paint = paint; it->color = theme.text; it->font = FONT_BODY;
    if (rc->right > doc->content_width) doc->content_width = rc->right;
    return (int)doc->count++;
}

// MARK: - Text

static void paint_text(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    RECT r = *rc;
    draw_text(hdc, it->text, &r, it->font, it->color, it->flags);
}
int doc_text_at(Doc *doc, const RECT *rc, const char *text, FontId f, COLORREF color, UINT flags) {
    int i = doc_add(doc, rc, paint_text);
    Item *it = &doc->items[i];
    it->text = xstrdup(text ? text : ""); it->font = f; it->color = color; it->flags = flags;
    return i;
}
int doc_text(Doc *doc, int x, int w, const char *text, FontId f, COLORREF color, UINT flags) {
    bool single = (flags & DT_SINGLELINE) != 0;
    int h = single ? font_height(doc->hdc, f) : measure_text(doc->hdc, text, w, f, flags & ~DT_VCENTER);
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_text_at(doc, &rc, text, f, color, flags | (single ? 0 : DT_WORDBREAK | DT_EDITCONTROL));
    doc->y += h;
    return i;
}

// MARK: - Rich text

typedef struct { int x, y, w, h, line; FontId font; COLORREF color; bool code, link, strike; char *url; wchar_t *text; } Run;
typedef struct { Run *runs; size_t count, cap; int height; } Rich;

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
    free(r->runs); free(r);
}

static Run *rich_push(Rich *r) {
    if (r->count == r->cap) { r->cap = r->cap ? r->cap * 2 : 32; r->runs = xrealloc(r->runs, r->cap * sizeof *r->runs); }
    Run *run = &r->runs[r->count++];
    memset(run, 0, sizeof *run);
    return run;
}

/// Breaks spans into word runs that fit `width`, line by line.
static Rich *rich_layout(HDC hdc, const char *markdown, int width, FontId base, COLORREF color) {
    Rich *r = xcalloc(1, sizeof *r);
    size_t n; MdSpan *spans = md_inline(markdown, &n);
    int x = 0, line = 0;
    int pad = px(3);   // code span padding
    for (size_t s = 0; s < n; s++) {
        FontId f = rich_font(base, spans[s].flags);
        bool code = (spans[s].flags & SPAN_CODE) != 0, link = (spans[s].flags & SPAN_LINK) != 0 && spans[s].url, strike = (spans[s].flags & SPAN_STRIKE) != 0;
        COLORREF c = code ? theme.accent : link ? theme.accent : strike ? theme.secondary : color;
        wchar_t *text = utf8_to_wide(spans[s].text);
        HFONT old = SelectObject(hdc, font(f));
        TEXTMETRICW tm; GetTextMetricsW(hdc, &tm);
        int line_h = tm.tmHeight + (code ? pad : 0);
        const wchar_t *p = text;
        while (*p) {
            if (*p == L'\n') { x = 0; line++; p++; continue; }
            // A word with its trailing spaces.
            const wchar_t *end = p;
            if (code) { while (*end && *end != L'\n') end++; }
            else { while (*end && *end != L' ' && *end != L'\n') end++; }
            const wchar_t *fit_end = end;
            while (*end == L' ') end++;
            size_t len = (size_t)(end - p), fit_len = (size_t)(fit_end - p);
            if (fit_len == 0 && len > 0) fit_len = len;
            SIZE fit = { 0, 0 }, full = { 0, 0 };
            GetTextExtentPoint32W(hdc, p, (int)fit_len, &fit);
            GetTextExtentPoint32W(hdc, p, (int)len, &full);
            int extra = code ? pad * 2 : 0;
            if (x > 0 && x + fit.cx + extra > width) { x = 0; line++; }
            if (fit.cx + extra > width && fit_len > 1) {
                // Break a word longer than the line by characters.
                size_t k = 1;
                for (;;) {
                    int fitting = 0;
                    GetTextExtentExPointW(hdc, p, (int)fit_len, width - extra, &fitting, NULL, &fit);
                    k = fitting > 0 ? (size_t)fitting : 1;
                    if (k >= fit_len) break;
                    GetTextExtentPoint32W(hdc, p, (int)k, &fit);
                    Run *run = rich_push(r);
                    run->x = x; run->line = line; run->w = fit.cx + extra; run->h = line_h; run->font = f; run->color = c; run->code = code; run->link = link; run->strike = strike;
                    run->url = link ? xstrdup(spans[s].url) : NULL; run->text = xmalloc((k + 1) * sizeof(wchar_t)); memcpy(run->text, p, k * sizeof(wchar_t)); run->text[k] = 0;
                    p += k; fit_len -= k; len -= k; x = 0; line++;
                }
                GetTextExtentPoint32W(hdc, p, (int)len, &full);
            }
            Run *run = rich_push(r);
            run->x = x; run->line = line; run->w = full.cx + extra; run->h = line_h; run->font = f; run->color = c; run->code = code; run->link = link; run->strike = strike;
            run->url = link ? xstrdup(spans[s].url) : NULL;
            run->text = xmalloc((len + 1) * sizeof(wchar_t)); memcpy(run->text, p, len * sizeof(wchar_t)); run->text[len] = 0;
            x += run->w;
            p = end;
        }
        SelectObject(hdc, old);
        free(text);
    }
    md_spans_free(spans, n);
    // Line heights, then run positions.
    int lines = 0;
    for (size_t i = 0; i < r->count; i++) if (r->runs[i].line + 1 > lines) lines = r->runs[i].line + 1;
    if (!lines) lines = 1;
    int *heights = xcalloc((size_t)lines, sizeof *heights);
    int base_h = font_height(hdc, base) + px(3);
    for (int l = 0; l < lines; l++) heights[l] = base_h;
    for (size_t i = 0; i < r->count; i++) if (r->runs[i].h + px(3) > heights[r->runs[i].line]) heights[r->runs[i].line] = r->runs[i].h + px(3);
    int y = 0;
    for (int l = 0; l < lines; l++) {
        for (size_t i = 0; i < r->count; i++) if (r->runs[i].line == l) r->runs[i].y = y + (heights[l] - px(3) - r->runs[i].h);
        y += heights[l];
    }
    r->height = y;
    free(heights);
    return r;
}

static void paint_rich(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    Rich *r = it->data;
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) {
        Run *run = &r->runs[i];
        RECT rr = { rc->left + run->x, rc->top + run->y, rc->left + run->x + run->w, rc->top + run->y + run->h };
        if (run->code) {
            RECT bg = rr; bg.right = bg.left + run->w;
            // Trailing spaces stay outside the tint.
            size_t len = wcslen(run->text); while (len && run->text[len - 1] == L' ') len--;
            SIZE sz = { 0, 0 }; HFONT old = SelectObject(hdc, font(run->font)); GetTextExtentPoint32W(hdc, run->text, (int)len, &sz); SelectObject(hdc, old);
            bg.right = bg.left + sz.cx + px(6);
            fill_round_rect(hdc, &bg, px(4), theme.code, theme.code);
            rr.left += px(3);
        }
        draw_textw(hdc, run->text, &rr, run->font, run->color, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOCLIP);
        if (run->strike) {
            size_t len = wcslen(run->text); while (len && run->text[len - 1] == L' ') len--;
            SIZE sz = { 0, 0 }; HFONT old = SelectObject(hdc, font(run->font)); GetTextExtentPoint32W(hdc, run->text, (int)len, &sz); SelectObject(hdc, old);
            int mid = rr.bottom - run->h / 2;
            draw_line(hdc, rr.left, mid, rr.left + sz.cx, mid, run->color);
        }
        if (run->link) {
            size_t len = wcslen(run->text); while (len && run->text[len - 1] == L' ') len--;
            SIZE sz = { 0, 0 }; HFONT old = SelectObject(hdc, font(run->font)); GetTextExtentPoint32W(hdc, run->text, (int)len, &sz); SelectObject(hdc, old);
            draw_line(hdc, rr.left, rr.bottom - 1, rr.left + sz.cx, rr.bottom - 1, blend(theme.accent, theme.background, 0.6));
        }
    }
}
int doc_rich(Doc *doc, int x, int w, const char *markdown, FontId base, COLORREF color) {
    Rich *r = rich_layout(doc->hdc, markdown ? markdown : "", w, base, color);
    RECT rc = { x, doc->y, x + w, doc->y + r->height };
    int i = doc_add(doc, &rc, paint_rich);
    Item *it = &doc->items[i];
    it->data = r; it->free_data = rich_free; it->text = xstrdup(markdown ? markdown : "");
    bool has_link = false;
    for (size_t k = 0; k < r->count && !has_link; k++) has_link = r->runs[k].link;
    if (has_link) { it->action = ACTION_OPEN_LINK; it->hand = true; }
    doc->y += r->height;
    return i;
}
int doc_rich_height(Doc *doc, int w, const char *markdown, FontId base) {
    Rich *r = rich_layout(doc->hdc, markdown ? markdown : "", w, base, theme.text);
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

// MARK: - Boxes

static void paint_box(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    COLORREF fill = it->fill;
    if (it->action && it->hover_fill && doc->hover >= 0 && &doc->items[doc->hover] == it) fill = blend(theme.text, it->fill, theme.dark ? 0.06 : 0.04);
    if (it->action && doc->pressed >= 0 && &doc->items[doc->pressed] == it) fill = blend(theme.text, it->fill, 0.09);
    fill_round_rect(hdc, rc, it->radius, fill, it->border);
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

static void paint_rule(Doc *doc, Item *it, HDC hdc, const RECT *rc) { draw_line(hdc, rc->left, rc->top, rc->right, rc->top, it->color); }
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
static void paint_label(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    LabelData *d = it->data;
    RECT g = { rc->left, rc->top, rc->left + d->glyph_w, rc->top + font_height(hdc, it->font) + px(2) };
    draw_glyph(hdc, d->glyph, &g, d->glyph_font, it->color);
    RECT t = { rc->left + d->glyph_w + px(6), rc->top, rc->right, rc->bottom };
    draw_text(hdc, it->text, &t, it->font, it->color, it->flags);
}
int doc_label(Doc *doc, int x, int w, wchar_t glyph, const char *text, FontId f, COLORREF color) {
    LabelData *d = xcalloc(1, sizeof *d);
    d->glyph = glyph; d->glyph_font = FONT_ICON_SMALL; d->glyph_w = px(16);
    int h = measure_text(doc->hdc, text, w - d->glyph_w - px(6), f, DT_WORDBREAK);
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &rc, paint_label);
    Item *it = &doc->items[i];
    it->data = d; it->free_data = free; it->text = xstrdup(text ? text : ""); it->font = f; it->color = color; it->flags = DT_WORDBREAK | DT_EDITCONTROL;
    doc->y += h;
    return i;
}
int doc_notice(Doc *doc, int x, int w, const char *message) { return doc_label(doc, x, w, 0xE7BA, message, FONT_CALLOUT, theme.danger); }
int doc_notice_box(Doc *doc, int x, int w, const char *message) {
    COLORREF fill = blend(theme.danger, theme.background, 0.08);
    int box = doc_box_begin(doc, x, w, px(12), fill, fill, px(12));
    doc->items[box].hover_fill = false;
    doc_notice(doc, x + px(12), w - px(24), message);
    doc_box_end(doc, box, px(12));
    return box;
}

// MARK: - Buttons

typedef struct { ButtonStyle style; bool enabled; } ButtonData;
static void paint_button(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    ButtonData *d = it->data;
    bool hovered = doc->hover >= 0 && &doc->items[doc->hover] == it && d->enabled;
    bool pressed = doc->pressed >= 0 && &doc->items[doc->pressed] == it && d->enabled;
    COLORREF fill, border, text;
    switch (d->style) {
    case BUTTON_PROMINENT:
        fill = d->enabled ? theme.accent : blend(theme.secondary, theme.background, 0.35); border = fill; text = theme.white;
        if (hovered) fill = border = blend(theme.text, theme.accent, 0.1);
        break;
    case BUTTON_DESTRUCTIVE:
        fill = blend(theme.danger, theme.background, hovered ? 0.16 : 0.1); border = blend(theme.danger, theme.background, 0.3); text = theme.danger; break;
    case BUTTON_PLAIN:
        fill = hovered ? blend(theme.accent, theme.background, 0.08) : theme.background; border = fill; text = theme.accent; break;
    default:
        fill = hovered ? blend(theme.text, theme.surface, 0.05) : theme.surface; border = theme.border; text = theme.text; break;
    }
    if (pressed) fill = blend(theme.text, fill, 0.12);
    if (!d->enabled) text = blend(text, fill, 0.5);
    int h = rc->bottom - rc->top;
    fill_round_rect(hdc, rc, h / 2, fill, border);
    RECT t = *rc;
    draw_text(hdc, it->text, &t, FONT_SUBHEADLINE_SEMIBOLD, text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}
int doc_button(Doc *doc, int x, int w, const char *text, ButtonStyle style, int action, intptr_t arg, bool enabled) {
    int h = font_height(doc->hdc, FONT_SUBHEADLINE_SEMIBOLD) + px(14);
    int tw = text_width(doc->hdc, text, FONT_SUBHEADLINE_SEMIBOLD) + px(28);
    int width = w > 0 ? w : w < 0 ? doc->width - x : tw;
    RECT rc = { x, doc->y, x + width, doc->y + h };
    int i = doc_add(doc, &rc, paint_button);
    Item *it = &doc->items[i];
    ButtonData *d = xcalloc(1, sizeof *d); d->style = style; d->enabled = enabled;
    it->data = d; it->free_data = free; it->text = xstrdup(text);
    if (enabled) { it->action = action; it->arg = arg; it->hand = true; }
    doc->y += h;
    return i;
}

typedef struct { char **titles; size_t count; int selected; bool enabled; int action; intptr_t arg_base; } SegmentData;
static void segments_free(void *p) { SegmentData *d = p; str_array_free(d->titles, d->count); free(d); }
static void paint_segments(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    SegmentData *d = it->data;
    int h = rc->bottom - rc->top;
    fill_round_rect(hdc, rc, px(8), theme.surface, theme.surface);
    int w = (rc->right - rc->left) / (int)(d->count ? d->count : 1);
    for (size_t i = 0; i < d->count; i++) {
        RECT seg = { rc->left + (int)i * w + px(2), rc->top + px(2), rc->left + (int)(i + 1) * w - px(2), rc->bottom - px(2) };
        bool selected = (int)i == d->selected;
        if (selected) fill_round_rect(hdc, &seg, px(6), theme.elevated, theme.border);
        COLORREF c = d->enabled ? (selected ? theme.text : theme.secondary) : blend(theme.secondary, theme.surface, 0.5);
        draw_text(hdc, d->titles[i], &seg, selected ? FONT_CAPTION_SEMIBOLD : FONT_CAPTION_MEDIUM, c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    (void)h;
}
int doc_segments(Doc *doc, int x, int w, const char *const *titles, size_t count, int selected, int action, intptr_t arg_base, bool enabled) {
    int h = font_height(doc->hdc, FONT_CAPTION_SEMIBOLD) + px(14);
    RECT rc = { x, doc->y, x + w, doc->y + h };
    int i = doc_add(doc, &rc, paint_segments);
    Item *it = &doc->items[i];
    SegmentData *d = xcalloc(1, sizeof *d);
    d->titles = xmalloc((count ? count : 1) * sizeof *d->titles);
    for (size_t k = 0; k < count; k++) d->titles[k] = xstrdup(titles[k]);
    d->count = count; d->selected = selected; d->enabled = enabled; d->action = action; d->arg_base = arg_base;
    it->data = d; it->free_data = segments_free;
    if (enabled) { it->action = action; it->arg = arg_base; it->hand = true; }
    doc->y += h;
    return i;
}
/// Segment index at a point; used by the pane to resolve the argument.
static int segment_at(Item *it, int x) {
    SegmentData *d = it->data;
    if (!d->count) return 0;
    int w = (it->rc.right - it->rc.left) / (int)d->count;
    int idx = (x - it->rc.left) / (w ? w : 1);
    if (idx < 0) idx = 0;
    if ((size_t)idx >= d->count) idx = (int)d->count - 1;
    return idx;
}

int doc_loading(Doc *doc, int x, int w, const char *text) {
    doc_space(doc, px(16));
    int i = doc_text(doc, x, w, text ? text : "Loading\xE2\x80\xA6", FONT_CALLOUT, theme.secondary, DT_CENTER);
    doc_space(doc, px(16));
    return i;
}

typedef struct { wchar_t glyph; } EmptyData;
static void paint_empty_glyph(Doc *doc, Item *it, HDC hdc, const RECT *rc) { draw_glyph(hdc, ((EmptyData *)it->data)->glyph, rc, FONT_ICON_HUGE, theme.tertiary); }
int doc_empty_state(Doc *doc, int x, int w, wchar_t glyph, const char *title, const char *detail) {
    doc_space(doc, px(40));
    EmptyData *d = xcalloc(1, sizeof *d); d->glyph = glyph;
    int first = doc_custom(doc, x, w, px(44), paint_empty_glyph, d, free, 0, 0);
    doc_space(doc, px(8));
    doc_text(doc, x, w, title, FONT_HEADLINE, theme.secondary, DT_CENTER);
    if (detail) { doc_space(doc, px(4)); doc_text(doc, x + w / 6, w * 2 / 3, detail, FONT_FOOTNOTE, theme.secondary, DT_CENTER); }
    doc_space(doc, px(24));
    return first;
}

typedef struct { char *value; COLORREF value_color; int label_w; } LabeledData;
static void labeled_free(void *p) { LabeledData *d = p; free(d->value); free(d); }
static void paint_labeled(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    LabeledData *d = it->data;
    RECT l = { rc->left, rc->top, rc->left + d->label_w, rc->bottom };
    draw_text(hdc, it->text, &l, FONT_CALLOUT, theme.text, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT v = { rc->left + d->label_w + px(12), rc->top, rc->right, rc->bottom };
    draw_text(hdc, d->value, &v, FONT_CALLOUT, d->value_color, DT_RIGHT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL);
}
int doc_labeled(Doc *doc, int x, int w, const char *label, const char *value, COLORREF value_color) {
    int label_w = text_width(doc->hdc, label, FONT_CALLOUT);
    if (label_w > w / 2) label_w = w / 2;
    int h = measure_text(doc->hdc, value, w - label_w - px(12), FONT_CALLOUT, DT_WORDBREAK);
    int lh = font_height(doc->hdc, FONT_CALLOUT);
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
static int badge_width(HDC hdc, const BadgeSpec *b, const char *text, int *h) {
    if (b->chip) {
        *h = font_height(hdc, FONT_CAPTION2) + px(6);
        return px(7) * 2 + px(6) + px(4) + text_width(hdc, text, FONT_CAPTION_MEDIUM);
    }
    *h = font_height(hdc, FONT_CAPTION2) + px(7);
    int glyph_w = b->glyph ? px(11) : 0;
    return px(8) * 2 + glyph_w + (b->glyph && text && *text ? px(3) : 0) + text_width(hdc, text, FONT_CAPTION_SEMIBOLD);
}
static void paint_badges(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    BadgesData *d = it->data;
    for (size_t i = 0; i < d->count; i++) {
        int x = rc->left + d->rects[i].left, y = rc->top + d->rects[i].top;
        if (d->specs[i].chip) draw_chip(hdc, x, y, d->texts[i], d->specs[i].color, d->background, NULL);
        else draw_badge(hdc, x, y, d->specs[i].glyph, d->texts[i], d->specs[i].color, d->background, NULL);
    }
}
int doc_badges(Doc *doc, int x, int w, const BadgeSpec *badges, size_t count, COLORREF background) {
    BadgesData *d = xcalloc(1, sizeof *d);
    d->specs = xmalloc((count ? count : 1) * sizeof *d->specs); d->texts = xmalloc((count ? count : 1) * sizeof *d->texts);
    d->rects = xmalloc((count ? count : 1) * sizeof *d->rects); d->count = count; d->background = background;
    int cx = 0, cy = 0, row_h = 0, gap = px(5);
    for (size_t i = 0; i < count; i++) {
        d->specs[i] = badges[i]; d->texts[i] = xstrdup(badges[i].text ? badges[i].text : "");
        int h, bw = badge_width(doc->hdc, &badges[i], d->texts[i], &h);
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

int doc_section(Doc *doc, int x, int w, const char *title) {
    doc_space(doc, px(14));
    int i = doc_text(doc, x + px(4), w - px(8), title, FONT_CAPTION_SEMIBOLD, theme.secondary, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    return i;
}

// MARK: - Markdown

typedef struct { char *language; char *code; bool copied; } CodeData;
static void code_free(void *p) { CodeData *d = p; free(d->language); free(d->code); free(d); }
static void doc_table(Doc *doc, int x, int w, const MdBlock *b, FontId base);
static void paint_task_box(Doc *doc, Item *it, HDC hdc, const RECT *rc);
static void paint_code_header(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    CodeData *d = it->data;
    RECT l = { rc->left + px(12), rc->top, rc->right - px(40), rc->bottom };
    draw_text(hdc, d->language ? d->language : "code", &l, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT g = { rc->right - px(34), rc->top, rc->right - px(6), rc->bottom };
    bool hovered = doc->hover >= 0 && &doc->items[doc->hover] == it;
    if (hovered) fill_round_rect(hdc, &g, px(6), blend(theme.text, theme.code, 0.06), blend(theme.text, theme.code, 0.06));
    draw_glyph(hdc, d->copied ? 0xE73E : 0xE8C8, &g, FONT_ICON_SMALL, theme.secondary);
    draw_line(hdc, rc->left, rc->bottom - 1, rc->right, rc->bottom - 1, theme.border);
}
static void paint_code_text(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    RECT r = *rc;
    draw_text(hdc, it->text, &r, FONT_MONO_SMALL, theme.text, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_EDITCONTROL | DT_EXPANDTABS);
}
static void quote_bar(Doc *doc, Item *it, HDC hdc, const RECT *rc) { RECT r = { rc->left, rc->top, rc->left + px(3), rc->bottom }; fill_round_rect(hdc, &r, px(1), theme.border, theme.border); }
static void paint_task_box(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    bool checked = it->arg != 0;
    fill_round_rect(hdc, rc, px(3), checked ? theme.accent : theme.elevated, checked ? theme.accent : blend(theme.text, theme.background, 0.35));
    if (checked) draw_glyph(hdc, 0xE73E, rc, FONT_ICON_SMALL, theme.white);
}

// MARK: - Tables

typedef struct { Rich **cells; size_t rows, cols; int *col_x, *col_w, *row_y, *row_h; char *aligns; } TableData;
static void table_free(void *p) {
    TableData *t = p;
    for (size_t i = 0; i < t->rows * t->cols; i++) rich_free(t->cells[i]);
    free(t->cells); free(t->col_x); free(t->col_w); free(t->row_y); free(t->row_h); free(t->aligns); free(t);
}
static void paint_table(Doc *doc, Item *it, HDC hdc, const RECT *rc) {
    TableData *t = it->data;
    int pad = px(8);
    // Header tint, zebra rows and a grid.
    for (size_t r = 0; r < t->rows; r++) {
        RECT row = { rc->left, rc->top + t->row_y[r], rc->right, rc->top + t->row_y[r] + t->row_h[r] };
        if (r == 0) fill_rect(hdc, &row, theme.surface);
        else if (r % 2 == 0) fill_rect(hdc, &row, blend(theme.text, theme.background, 0.02));
        draw_line(hdc, rc->left, row.bottom, rc->right, row.bottom, theme.border);
    }
    draw_line(hdc, rc->left, rc->top, rc->right, rc->top, theme.border);
    for (size_t c = 0; c <= t->cols; c++) {
        int x = c < t->cols ? rc->left + t->col_x[c] : rc->right - 1;
        draw_line(hdc, x, rc->top, x, rc->bottom, theme.border);
    }
    for (size_t r = 0; r < t->rows; r++) {
        for (size_t c = 0; c < t->cols; c++) {
            Rich *cell = t->cells[r * t->cols + c];
            int cell_w = t->col_w[c] - 2 * pad;
            int shift = 0;
            if (t->aligns[c] != 'l') {
                int widest = 0;
                for (size_t k = 0; k < cell->count; k++) if (cell->runs[k].x + cell->runs[k].w > widest) widest = cell->runs[k].x + cell->runs[k].w;
                shift = t->aligns[c] == 'r' ? cell_w - widest : (cell_w - widest) / 2;
                if (shift < 0) shift = 0;
            }
            RECT cr = { rc->left + t->col_x[c] + pad + shift, rc->top + t->row_y[r] + px(5), rc->left + t->col_x[c] + t->col_w[c] - pad, rc->top + t->row_y[r] + t->row_h[r] };
            Item fake; memset(&fake, 0, sizeof fake); fake.data = cell;
            paint_rich(doc, &fake, hdc, &cr);
        }
    }
}
/// A table sized to its content: columns take what their widest cell wants, then share the width that is left.
static void doc_table(Doc *doc, int x, int w, const MdBlock *b, FontId base) {
    TableData *t = xcalloc(1, sizeof *t);
    t->rows = b->rows; t->cols = b->cols; t->aligns = xstrdup(b->aligns);
    t->cells = xcalloc(t->rows * t->cols, sizeof *t->cells);
    t->col_x = xcalloc(t->cols, sizeof *t->col_x); t->col_w = xcalloc(t->cols, sizeof *t->col_w);
    t->row_y = xcalloc(t->rows, sizeof *t->row_y); t->row_h = xcalloc(t->rows, sizeof *t->row_h);
    int pad = px(8);
    FontId cell_font = base == FONT_BODY ? FONT_CALLOUT : base;
    // Natural widths, measured unwrapped.
    int *want = xcalloc(t->cols, sizeof *want);
    for (size_t r = 0; r < t->rows; r++)
        for (size_t c = 0; c < t->cols; c++) {
            Rich *probe = rich_layout(doc->hdc, b->cells[r * t->cols + c], 100000, r == 0 ? rich_font(cell_font, SPAN_BOLD) : cell_font, theme.text);
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
    for (size_t c = 0; c < t->cols; c++) { t->col_x[c] = cx; t->col_w[c] = want[c]; cx += want[c]; }
    int table_w = cx < w ? cx : w;
    int y = 1;
    for (size_t r = 0; r < t->rows; r++) {
        int h = 0;
        for (size_t c = 0; c < t->cols; c++) {
            int cell_w = t->col_w[c] - 2 * pad; if (cell_w < px(16)) cell_w = px(16);
            Rich *cell = rich_layout(doc->hdc, b->cells[r * t->cols + c], cell_w, r == 0 ? rich_font(cell_font, SPAN_BOLD) : cell_font, theme.text);
            t->cells[r * t->cols + c] = cell;
            if (cell->height > h) h = cell->height;
        }
        t->row_y[r] = y; t->row_h[r] = h + px(10);
        y += t->row_h[r] + 1;
    }
    free(want);
    RECT rc = { x, doc->y, x + table_w, doc->y + y };
    int i = doc_add(doc, &rc, paint_table);
    Item *it = &doc->items[i];
    it->data = t; it->free_data = table_free;
    doc->y += y;
}

void doc_markdown(Doc *doc, int x, int w, const char *source, FontId base) {
    size_t n; MdBlock *blocks = md_parse(source, &n);
    int gap = px(10);
    for (size_t i = 0; i < n; i++) {
        if (i) doc_space(doc, gap);
        MdBlock *b = &blocks[i];
        switch (b->kind) {
        case MD_PARAGRAPH: doc_rich(doc, x, w, b->text, base, theme.text); break;
        case MD_HEADING:
            doc_space(doc, px(4));
            doc_rich(doc, x, w, b->text, b->level == 1 ? FONT_TITLE3 : b->level == 2 ? FONT_HEADLINE : FONT_SUBHEADLINE_SEMIBOLD, theme.text);
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
                marker_w = text_width(doc->hdc, b->marker, base) + px(8);
                if (marker_w < px(18)) marker_w = px(18);
                RECT mr = { x + indent, top, x + indent + marker_w, top + font_height(doc->hdc, base) + px(3) };
                doc_text_at(doc, &mr, b->marker, base, theme.secondary, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            }
            doc_rich(doc, x + indent + marker_w, w - indent - marker_w, b->text, base, b->task == 2 ? theme.secondary : theme.text);
            break;
        }
        case MD_QUOTE: {
            int top = doc->y;
            doc_rich(doc, x + px(12), w - px(12), b->text, base, theme.secondary);
            RECT bar = { x, top, x + px(3), doc->y };
            doc_add(doc, &bar, quote_bar);
            break;
        }
        case MD_CODE: {
            int box = doc_box_begin(doc, x, w, 0, theme.code, theme.border, px(10));
            doc->items[box].hover_fill = false;
            int header_h = font_height(doc->hdc, FONT_MONO_CAPTION2) + px(12);
            RECT hr = { x, doc->y, x + w, doc->y + header_h };
            int header = doc_add(doc, &hr, paint_code_header);
            CodeData *d = xcalloc(1, sizeof *d); d->language = b->language ? xstrdup(b->language) : NULL; d->code = xstrdup(b->text);
            Item *hi = &doc->items[header];
            hi->data = d; hi->free_data = code_free; hi->action = ACTION_COPY_CODE; hi->arg = (intptr_t)d->code; hi->hand = true;
            doc->y += header_h;
            doc_space(doc, px(8));
            int th = measure_text(doc->hdc, b->text, w - px(24), FONT_MONO_SMALL, DT_WORDBREAK | DT_EXPANDTABS);
            RECT tr = { x + px(12), doc->y, x + w - px(12), doc->y + th };
            int ti = doc_add(doc, &tr, paint_code_text);
            doc->items[ti].text = xstrdup(b->text);
            doc->y += th;
            doc_box_end(doc, box, px(10));
            break;
        }
        case MD_RULE: doc_space(doc, px(4)); doc_rule(doc, x, w); doc_space(doc, px(4)); break;
        case MD_TABLE: doc_table(doc, x, w, b, base); break;
        }
    }
    md_free(blocks, n);
}

// MARK: - Paint and hit

void doc_paint(Doc *doc, HDC hdc, int scroll_x, int scroll_y, const RECT *clip) {
    for (size_t i = 0; i < doc->count; i++) {
        Item *it = &doc->items[i];
        RECT rc = { it->rc.left - scroll_x, it->rc.top - scroll_y, it->rc.right - scroll_x, it->rc.bottom - scroll_y };
        if (rc.bottom < clip->top - px(4) || rc.top > clip->bottom + px(4)) continue;
        if (rc.right < clip->left || rc.left > clip->right) continue;
        if (it->paint) it->paint(doc, it, hdc, &rc);
    }
}
int doc_hit(Doc *doc, int x, int y) {
    for (size_t i = doc->count; i-- > 0;) {
        Item *it = &doc->items[i];
        if (!it->action) continue;
        if (x >= it->rc.left && x < it->rc.right && y >= it->rc.top && y < it->rc.bottom) {
            if (it->paint == paint_segments) it->arg = ((SegmentData *)it->data)->arg_base + segment_at(it, x);
            if (it->paint == paint_rich && !doc_link_at(doc, (int)i, x, y)) continue;
            return (int)i;
        }
    }
    return -1;
}
char *doc_item_plain_text(Doc *doc, int index) {
    Item *it = doc_item(doc, index);
    if (!it || !it->text) return NULL;
    return it->paint == paint_rich ? md_plain(it->text) : xstrdup(it->text);
}
int doc_find(Doc *doc, int id) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].id == id) return (int)i;
    return -1;
}
