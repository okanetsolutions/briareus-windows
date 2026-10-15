#include "screen_dashboard_internal.h"

static void row_free(void *p);
static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

static void row_free(void *p) { RowData *d = p; for (int i = 0; i < d->n; i++) free(d->cell[i]); free(d->sub); free(d); }

static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = it->action && doc_item_hovered(doc, it);
    if (d->on || hovered) fill_rect(cv, rc, d->on ? theme.sunken : blend(theme.sunken, theme.raise, 0.6));
    if (!d->head) draw_line(cv, rc->left, rc->top, rc->right, rc->top, theme.line);
    int line = font_height(cv, d->font[0]);
    for (int i = 0; i < d->n; i++) {
        RECT c = { rc->left + d->x[i], rc->top, rc->left + d->x[i] + d->w[i], rc->bottom };
        if (i == 0 && d->swatch != CLR_INVALID) {
            int s = px(8), cy = (rc->top + rc->bottom) / 2;
            RECT sw = { c.left, cy - s / 2, c.left + s, cy - s / 2 + s };
            fill_round_rect(cv, &sw, px(2), d->swatch, d->swatch);
            c.left += s + px(8);
        }
        if (i == d->bar_col) {
            // `flex items-center gap-2`: the track takes what the number leaves.
            int nw = text_width(cv, d->cell[i], d->font[i]);
            RECT t = { c.right - nw, c.top, c.right, c.bottom };
            draw_text(cv, d->cell[i], &t, d->font[i], d->color[i], DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            int cy = (rc->top + rc->bottom) / 2, th = px(6);
            RECT track = { c.left, cy - th / 2, c.right - nw - px(8), cy - th / 2 + th };
            if (track.right - track.left > th) {
                fill_round_rect(cv, &track, th / 2, theme.sunken, theme.sunken);
                int fw = (int)((track.right - track.left) * d->bar + 0.5);
                if (fw > 0) { RECT fill = track; fill.right = track.left + (fw > th ? fw : th); fill_round_rect(cv, &fill, th / 2, d->bar_color, d->bar_color); }
            }
            continue;
        }
        if (i == 0 && d->sub) {
            int sub_h = font_height(cv, FONT_CAPTION), total = line + px(2) + sub_h;
            RECT a = { c.left, (rc->top + rc->bottom - total) / 2, c.right, 0 }; a.bottom = a.top + line;
            draw_text(cv, d->cell[i], &a, d->font[i], d->color[i], DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT b = { c.left, a.bottom + px(2), c.right, a.bottom + px(2) + sub_h };
            draw_text(cv, d->sub, &b, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            continue;
        }
        draw_text(cv, d->cell[i], &c, d->font[i], d->color[i], d->align[i] | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void screen_dashboard_table_init(Table *t, int n, const char *const *heads, const UINT *aligns) {
    memset(t, 0, sizeof *t);
    t->n = n;
    for (int i = 0; i < n; i++) { t->head[i] = heads[i]; t->align[i] = aligns[i]; }
    t->head_font = FONT_CAPTION2; t->head_color = theme.muted; t->row_h = px(30);
}

/// A new row with every cell in the body type; the caller sets the cells.
RowData *screen_dashboard_table_row(Table *t) {
    RowData *d = xcalloc(1, sizeof *d);
    d->n = t->n; d->swatch = CLR_INVALID; d->bar_col = -1;
    for (int i = 0; i < t->n; i++) { d->align[i] = t->align[i]; d->color[i] = theme.ink; d->font[i] = FONT_FOOTNOTE; }
    t->rows = xrealloc(t->rows, (t->count + 1) * sizeof *t->rows);
    t->rows[t->count++] = d;
    return d;
}

/// Lays the table out at the cursor: each column as wide as its widest cell, the flexible one taking the rest.
void screen_dashboard_table_emit(Doc *doc, int x, int w, Table *t, const int *actions, const intptr_t *args) {
    int width[COLS_MAX] = { 0 }, gap = px(12), used = 0;
    for (int i = 0; i < t->n; i++) {
        if (t->fixed[i]) { width[i] = w * t->fixed[i] / 100; continue; }
        if (i == t->flex) continue;
        int m = text_width(doc->cv, t->head[i], t->head_font);
        for (size_t r = 0; r < t->count; r++) { int cw = text_width(doc->cv, t->rows[r]->cell[i], t->rows[r]->font[i]); if (cw > m) m = cw; }
        width[i] = m;
    }
    for (int i = 0; i < t->n; i++) used += width[i] + (i < t->n - 1 ? gap : 0);
    width[t->flex] = w - used;
    if (width[t->flex] < px(80)) width[t->flex] = px(80);
    int xs[COLS_MAX], cx = 0;
    for (int i = 0; i < t->n; i++) { xs[i] = cx; cx += width[i] + gap; }
    RowData *head = xcalloc(1, sizeof *head);
    head->n = t->n; head->head = true; head->swatch = CLR_INVALID; head->bar_col = -1;
    for (int i = 0; i < t->n; i++) {
        head->x[i] = xs[i]; head->w[i] = width[i]; head->align[i] = t->align[i];
        head->cell[i] = xstrdup(t->head[i]); head->color[i] = t->head_color; head->font[i] = t->head_font;
    }
    doc_custom(doc, x, w, font_height(doc->cv, t->head_font) + px(10), paint_row, head, row_free, 0, 0);
    for (size_t r = 0; r < t->count; r++) {
        RowData *d = t->rows[r];
        for (int i = 0; i < t->n; i++) { d->x[i] = xs[i]; d->w[i] = width[i]; }
        int action = actions ? actions[r] : 0;
        doc_custom(doc, x, w, t->row_h, paint_row, d, row_free, action, args ? args[r] : 0);
    }
    free(t->rows); t->rows = NULL; t->count = 0;
}

void screen_dashboard_cell(RowData *d, int i, char *text) { free(d->cell[i]); d->cell[i] = text; }

/// The columns every breakdown ends with: sessions, turns, tokens in / out, (total,) time and cost.
void screen_dashboard_usage_cells(RowData *d, int from, const Json *u, bool total) {
    screen_dashboard_cell(d, from, xstrfmt("%d", screen_dashboard_count_of(u, "sessions")));
    screen_dashboard_cell(d, from + 1, xstrfmt("%d", screen_dashboard_count_of(u, "turns")));
    char *in = screen_dashboard_tokens_text(screen_dashboard_num(u, "inputTokens")), *out = screen_dashboard_tokens_text(screen_dashboard_num(u, "outputTokens"));
    screen_dashboard_cell(d, from + 2, xstrfmt("%s / %s", in, out)); free(in); free(out);
    int k = from + 3;
    if (total) screen_dashboard_cell(d, k++, screen_dashboard_tokens_text(screen_dashboard_num(u, "totalTokens")));
    screen_dashboard_cell(d, k, screen_dashboard_duration_or_dash(screen_dashboard_num(u, "durationMs")));
    screen_dashboard_cell(d, k + 1, screen_dashboard_cost_or_dash(u));
}
