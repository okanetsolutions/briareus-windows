#include "screen_dashboard_internal.h"

// MARK: - Bars

/// One column of a chart; hovering it names its bucket in the chart's heading, as the web's title tooltip does.
typedef struct { int chart; double value, max; char *tip; } BarData;

typedef struct { int chart; char *title; } ChartTitle;

static void bar_free(void *p);
static void title_free(void *p);
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_chart_title(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_axis_label(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static COLORREF series_color(int i);
static void paint_ring(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

static void bar_free(void *p) { BarData *d = p; free(d->tip); free(d); }

static void title_free(void *p) { ChartTitle *d = p; free(d->title); free(d); }

static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    BarData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_rect(cv, rc, blend(theme.ink, theme.raise, 0.04));
    if (d->value <= 0 || d->max <= 0) return;
    int full = rc->bottom - rc->top, h = (int)(d->value / d->max * full + 0.5);
    if (h < px(3)) h = px(3);
    RECT bar = { rc->left, rc->bottom - h, rc->right, rc->bottom };
    COLORREF color = hovered ? theme.accent : blend(theme.accent, theme.raise, 0.75);
    int r = px(4);
    if (rc->right - rc->left < r * 2 || h < r * 2) { fill_rect(cv, &bar, color); return; }
    fill_round_rect(cv, &bar, r, color, color);
    RECT square = { bar.left, bar.top + r, bar.right, bar.bottom };   // `rounded-t-[4px]`: only the top is rounded
    fill_rect(cv, &square, color);
}

static void paint_chart_title(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ChartTitle *d = it->data;
    RECT t = *rc;
    draw_text(cv, d->title, &t, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Item *h = doc->hover >= 0 ? doc_item(doc, doc->hover) : NULL;
    if (h && h->paint == paint_bar && ((BarData *)h->data)->chart == d->chart) {
        RECT r = *rc;
        draw_text(cv, ((BarData *)h->data)->tip, &r, FONT_CAPTION, theme.ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void paint_axis_label(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RECT t = *rc;
    draw_text(cv, it->text, &t, FONT_CAPTION2, theme.muted, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
}

/// bucketChart: one bar per bucket the server's calendar has reached, `metric` high. False when there is nothing to plot.
bool screen_dashboard_chart(Doc *doc, int x, int w, const Json *u, int chart_id, const char *metric, const char *title) {
    const Json *buckets = json_get(u, "buckets");
    bool month = str_eq(json_str(json_get(u, "unit")), "month");
    const char *today = json_str(json_get(u, "today"));
    char ahead[16] = "";
    if (today) snprintf(ahead, month ? 8 : sizeof ahead, "%s", today);
    size_t n = json_count(buckets), visible = 0;
    double max = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *b = json_at(buckets, i);
        const char *date = json_str(json_get(b, "date"));
        if (*ahead && date && strcmp(date, ahead) > 0) continue;
        visible++;
        if (screen_dashboard_num(b, metric) > max) max = screen_dashboard_num(b, metric);
    }
    if (!visible || max <= 0) return false;
    doc_space(doc, px(12));
    int pad = px(12), ix = x + px(14), iw = w - px(28);
    int box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    ChartTitle *t = xcalloc(1, sizeof *t); t->chart = chart_id; t->title = xstrdup(title);
    doc_custom(doc, ix, iw, font_height(doc->cv, FONT_CAPTION) + px(2), paint_chart_title, t, title_free, 0, 0);
    doc_space(doc, px(10));
    int gap = px(2), plot = px(112), top = doc->y;
    int every = month ? (n > 12 ? 3 : 1) : 7;
    double bw = (double)(iw - gap * ((int)visible - 1)) / (double)visible;
    int label_y = top + plot + px(4), label_w = px(70);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        const Json *b = json_at(buckets, i);
        const char *date = json_str(json_get(b, "date"));
        if (*ahead && date && strcmp(date, ahead) > 0) continue;
        int left = ix + (int)(k * (bw + gap)), right = ix + (int)(k * (bw + gap) + bw);
        if (right <= left) right = left + 1;
        char *name = screen_dashboard_bucket_name(date, month);
        int turns = screen_dashboard_count_of(b, "turns");
        Str tip; str_init(&tip);
        str_appendf(&tip, "%s: ", name);
        if (!turns) str_appendz(&tip, "no usage");
        else {
            char *tok = screen_dashboard_tokens_text(screen_dashboard_num(b, "totalTokens")), *cost = screen_dashboard_cost_text(b);
            str_appendf(&tip, "%s tok \xC2\xB7 %d turn%s", tok, turns, screen_dashboard_plural(turns));
            if (cost) str_appendf(&tip, " \xC2\xB7 %s", cost);
            free(tok); free(cost);
        }
        BarData *d = xcalloc(1, sizeof *d);
        d->chart = chart_id; d->value = screen_dashboard_num(b, metric); d->max = max; d->tip = str_detach(&tip);
        RECT rc = { left, top, right, top + plot };
        int bi = doc_add(doc, &rc, paint_bar);
        Item *it = doc_item(doc, bi);
        it->data = d; it->free_data = bar_free; it->action = ACT_BAR;
        // A label under every nth column, and none that would run off the right edge.
        if (k % every == 0 && (k == 0 || ix + iw - left >= label_w)) {
            RECT lr = { left, label_y, left + label_w < ix + iw ? left + label_w : ix + iw, label_y + font_height(doc->cv, FONT_CAPTION2) };
            int li = doc_add(doc, &lr, paint_axis_label);
            doc_item(doc, li)->text = xstrdup(name);
        }
        free(name);
        k++;
    }
    doc->y = label_y + font_height(doc->cv, FONT_CAPTION2);
    doc_box_end(doc, box, pad);
    return true;
}

static COLORREF series_color(int i) {
    unsigned h = briareus_chart_series_color(i);
    return RGB((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
}

bool screen_dashboard_share_series(const Json *rows, Ring *ring, COLORREF *colors) {
    size_t n = json_count(rows);
    double total = 0;
    for (size_t i = 0; i < n; i++) { colors[i] = CLR_INVALID; total += screen_dashboard_num(json_at(rows, i), "totalTokens"); }
    memset(ring, 0, sizeof *ring);
    if (total <= 0) return false;
    int spent = 0; double tail = 0;
    for (size_t i = 0; i < n; i++) {
        double t = screen_dashboard_num(json_at(rows, i), "totalTokens");
        if (t <= 0) continue;
        if (spent < SERIES_SLOTS) { colors[i] = series_color(spent); ring->share[spent] = t / total; ring->color[spent] = colors[i]; }
        else { colors[i] = series_color(SERIES_SLOTS); tail += t; }
        spent++;
    }
    ring->count = spent < SERIES_SLOTS ? spent : SERIES_SLOTS;
    if (tail > 0) { ring->share[ring->count] = tail / total; ring->color[ring->count] = series_color(SERIES_SLOTS); ring->count++; }
    if (ring->count < 3) { for (size_t i = 0; i < n; i++) colors[i] = CLR_INVALID; return false; }
    return true;
}

static void paint_ring(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    Ring *ring = it->data;
    // The web's 100-unit viewBox: radius 40, stroke 13, a 2-unit gap of surface between the arcs, starting at twelve.
    int size = rc->right - rc->left, cx = rc->left + size / 2, cy = rc->top + size / 2;
    double scale = size / 100.0, gap = 2.0 / (2 * 3.14159265358979 * 40) * 360;
    int r = (int)(40 * scale + 0.5), stroke = (int)(13 * scale + 0.5);
    double at = 0;
    for (int i = 0; i < ring->count; i++) {
        double sweep = ring->share[i] * 360 - gap;
        stroke_arc(cv, cx, cy, r, ring->color[i], stroke, -90 + at * 360, sweep > 0 ? sweep : 0);
        at += ring->share[i];
    }
}

/// The ring and its caption, `132px` square; advances.
void screen_dashboard_ring_at(Doc *doc, int x, const Ring *ring) {
    int size = px(132);
    Ring *copy = xmalloc(sizeof *copy); *copy = *ring;
    doc_custom(doc, x, size, size, paint_ring, copy, free, 0, 0);
    doc_space(doc, px(6));
    doc_text(doc, x, size, "share of tokens", FONT_CAPTION2, theme.muted, DT_CENTER | DT_SINGLELINE);
}
