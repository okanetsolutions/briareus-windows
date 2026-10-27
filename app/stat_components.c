#include "stat_components.h"
#include "briareus_tokens.h"
#include "str.h"
#include <stdlib.h>

static COLORREF tone_color(StatNumber fraction) {
    switch (stats_tone(fraction)) {
    case STAT_OK: return theme.success;
    case STAT_WARN: return theme.warning;
    case STAT_DANGER: return theme.danger;
    default: return theme.muted;
    }
}
static void paint_gauge(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; StatNumber *fraction = it->data;
    fill_round_rect(cv, rc, px(2), theme.line, theme.line);
    RECT fill = *rc; fill.right = fill.left + (int)((rc->right - rc->left) * fraction->value);
    if (fill.right > fill.left) fill_round_rect(cv, &fill, px(2), tone_color(*fraction), tone_color(*fraction));
}
void doc_stat_gauge(Doc *doc, int x, int w, StatNumber fraction) {
    if (!fraction.present) return;
    StatNumber *copy = xmalloc(sizeof *copy); *copy = fraction;
    doc_custom(doc, x, w, px(4), paint_gauge, copy, free, 0, 0);
}
void doc_stat_tiles(Doc *doc, int x, int w, const StatTile *tiles, size_t count) {
    int cols = w >= px(640) ? 4 : w >= px(320) ? 2 : 1;
    int gap = px(8), pad = px(12), tw = (w - gap * (cols - 1)) / cols;
    for (size_t start = 0; start < count; start += (size_t)cols) {
        int top = doc->y, bottom = top, boxes[4], n = 0;
        for (int k = 0; k < cols && start + (size_t)k < count; k++) {
            const StatTile *tile = &tiles[start + (size_t)k]; int tx = x + k * (tw + gap);
            doc->y = top;
            int box = doc_box_begin(doc, tx, tw, pad, theme.raise, theme.line, px(12));
            int ix = tx + pad, iw = tw - pad * 2;
            doc_text(doc, ix, iw, tile->label, FONT_CAPTION, theme.muted, DT_SINGLELINE | DT_END_ELLIPSIS);
            doc_space(doc, px(4));
            doc_text(doc, ix, iw, tile->value, FONT_STAT, theme.ink, DT_SINGLELINE | DT_END_ELLIPSIS);
            doc_text(doc, ix, iw, tile->detail, FONT_CAPTION, theme.muted, DT_WORDBREAK);
            doc_space(doc, px(6)); doc_stat_gauge(doc, ix, iw, tile->fraction);
            doc_box_end(doc, box, pad); boxes[n++] = box;
            if (doc->y > bottom) bottom = doc->y;
        }
        for (int k = 0; k < n; k++) doc_item(doc, boxes[k])->rc.bottom = bottom;
        doc->y = bottom + gap;
    }
}
static void series_free(void *p) { StatsSeries *s = p; stats_series_free(s); free(s); }
static void paint_series(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; StatsSeries *s = it->data;
    int left = rc->left + px(82), right = rc->right, top = rc->top, bottom = rc->bottom - px(22);
    if (right <= left || bottom <= top) return;
    double maximum = s->maximum > 0 ? s->maximum : 1;
    for (int i = 0; i <= 3; i++) {
        int y = bottom - (bottom - top) * i / 3;
        draw_line(cv, left, y, right, y, theme.line);
        char *label = stats_axis(s->unit, (StatNumber){maximum * ((double)i / 3), true});
        RECT text = {rc->left, y - px(7), left - px(6), y + px(9)};
        draw_text(cv, label, &text, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_SINGLELINE); free(label);
    }
    unsigned rgb = briareus_chart_series_color(s->slot);
    COLORREF color = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    bool previous = false; int old_x = 0, old_y = 0;
    for (size_t i = 0; i < s->count; i++) {
        StatsPoint p = s->points[i];
        if (!p.number.present) { previous = false; continue; }
        double share = (p.at_ms - s->first_ms) / (s->last_ms - s->first_ms);
        int x = left + (int)(share * (right - left));
        int y = bottom - (int)(p.number.value / maximum * (bottom - top));
        if (previous) draw_thick_line(cv, old_x, old_y, x, y, color, px(2));
        else draw_line(cv, x, y, x + 1, y, color);
        previous = true; old_x = x; old_y = y;
    }
    char *first = stats_time((StatNumber){s->first_ms, true}), *last = stats_time((StatNumber){s->last_ms, true});
    RECT begin = {left, bottom + px(6), right, rc->bottom}, end = begin;
    draw_text(cv, first, &begin, FONT_CAPTION2, theme.muted, DT_LEFT | DT_SINGLELINE);
    draw_text(cv, last, &end, FONT_CAPTION2, theme.muted, DT_RIGHT | DT_SINGLELINE);
    free(first); free(last);
}
void doc_stats_chart(Doc *doc, int x, int w, StatsSeries series) {
    int pad = px(12), box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    int ix = x + pad, iw = w - pad * 2;
    char *latest = stats_axis(series.unit, series.latest);
    char *title = xstrfmt("%s · %s %s", series.title, series.label, latest);
    doc_text(doc, ix, iw, title, FONT_SUBHEADLINE, theme.ink, DT_WORDBREAK);
    free(latest); free(title); doc_space(doc, px(8));
    if (series.valid_count < 2 || series.last_ms <= series.first_ms) {
        doc_text(doc, ix, iw, "The history fills in as the server samples.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        stats_series_free(&series);
    } else {
        StatsSeries *copy = xmalloc(sizeof *copy); *copy = series;
        doc_custom(doc, ix, iw, px(162), paint_series, copy, series_free, 0, 0);
    }
    doc_box_end(doc, box, pad); doc_space(doc, px(8));
}
