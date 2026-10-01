// The 📊 Dashboard: what every project spent over a window, as the web dashboard's home pane draws it from the usage
// ledger (`GET /usage/all`). The totals as tiles, tokens and cost per day or month as bars, the most expensive sessions,
// and the spend by project, activity, provider and model, each with a ring of its share of the tokens. The window and
// six filters narrow every number on the page; a row of a breakdown is a filter too. Reading it needs an Admin token.
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_PERIOD = 1000, ACT_REFRESH, ACT_FILTER, ACT_CLEAR, ACT_ROW, ACT_OPEN_SESSION, ACT_BAR };
enum { TIMER_POLL = 1 };
enum { ROW_STRIDE = 100000 };   // a breakdown row's argument is its filter times the stride plus its index
enum { MENU_CAP = 60 };         // the most options a picker lists; the window's sessions can run to thousands

typedef enum { F_PROJECT, F_MODEL, F_ACTIVITY, F_PROVIDER, F_ACCOUNT, F_SESSION, F_COUNT } FilterId;
static const struct { const char *param, *one, *plural, *options; bool multi; } FILTERS[F_COUNT] = {
    { "project", "project", "projects", "projects", true },
    { "model", "model", "models", "models", true },
    { "activity", "activity", "activities", "activities", false },
    { "provider", "provider", "providers", "providers", false },
    { "account", "account", "accounts", "accounts", false },
    { "session", "session", "sessions", "sessions", false },
};
static const struct { const char *id, *label; } PERIODS[] = {
    { "month", "This month" }, { "prev", "Last month" }, { "all", "All time" },
    { "today", "Today" }, { "7d", "Last 7 days" }, { "30d", "Last 30 days" },
};
enum { PERIOD_COUNT = sizeof PERIODS / sizeof *PERIODS };

// The window is a preference, kept while the app runs; the filters are not, so a fresh visit shows everything.
static int g_period = 0;

/// The dashboard's ACTIVITY_LABELS: what each kind of session is called on its row.
static const struct { const char *id, *label; } ACTIVITIES[] = {
    { "chat", "\xF0\x9F\x92\xAC Chat" }, { "preview", "\xE2\x96\xB6 Run" }, { "code-review", "\xE2\x8C\x95 Code review" },
    { "issue", "\xE2\x96\xB6 Issue" }, { "qa", "\xF0\x9F\x94\x8D QA" }, { "orchestrator", "\xF0\x9F\xA7\xAD Orchestrator" },
    { "worker", "\xF0\x9F\x91\xB7 Worker" }, { "zeus", "\xE2\x9A\xA1 Zeus" }, { "analyst", "\xF0\x9F\x94\xAC Analyst" },
    { "pr-body-summary", "\xE2\x9C\x8E PR body summary" }, { "test-sheet", "\xF0\x9F\x93\x8B Test sheet" },
    { "test-run", "\xF0\x9F\x8E\xAC Test run" }, { "solve-conflicts", "\xF0\x9F\x94\x80 Solve conflicts" },
    { "fix-checks", "\xF0\x9F\xA7\xAA Fix failing checks" }, { "implement-feedback", "\xF0\x9F\x9B\xA0 Implement feedback" },
    { "custom-feedback", "\xE2\x9C\x8D Give feedback" }, { "delete-self-comments", "\xF0\x9F\xA7\xB9 Delete my comments" },
};
static const char *activity_label(const char *id) {
    if (str_empty(id) || str_eq(id, "unknown")) return "Unattributed";
    for (size_t i = 0; i < sizeof ACTIVITIES / sizeof *ACTIVITIES; i++) if (str_eq(ACTIVITIES[i].id, id)) return ACTIVITIES[i].label;
    return id;
}

typedef struct {
    Screen base;
    Json *data;                       // the payload on screen
    char *data_key;                   // the query it answered: a pick shows the loader until its own answer lands
    Json *options;                    // what the pickers offer, from the last payload: the whole window's, never the pick's
    char **picks[F_COUNT]; size_t pick_count[F_COUNT];
    char *error;
    Request *req;
    Poller poller;
} DashboardScreen;

// MARK: - Numbers as the dashboard words them

static double num(const Json *u, const char *key) { return json_num_or(json_get(u, key), 0); }
static int count_of(const Json *u, const char *key) { return json_int_or(json_get(u, key), 0); }
static const char *plural(int n) { return n == 1 ? "" : "s"; }

/// fmtTokens: "21599.7M", "80.5M", "93.0k".
static char *tokens_text(double n) {
    if (n >= 1e6) return xstrfmt("%.1fM", n / 1e6);
    if (n >= 1000) return xstrfmt("%.1fk", n / 1000);
    return xstrfmt("%.0f", n);
}
/// fmtCost: one price, and a `+` when some turns carry none, so the total is a floor. NULL when nothing was priced.
static char *cost_value(const Json *u, const char *key) {
    double usd;
    if (!json_num(json_get(u, key), &usd)) return NULL;
    return xstrfmt("$%.2f%s", usd, count_of(u, "unpricedTurns") ? "+" : "");
}
static char *cost_text(const Json *u) { return cost_value(u, "costUsd"); }
static char *cost_or_dash(const Json *u) { char *c = cost_text(u); return c ? c : xstrdup("\xE2\x80\x94"); }
static char *cost_note(const Json *u) {
    int unpriced = count_of(u, "unpricedTurns"), turns = count_of(u, "turns");
    if (unpriced) return xstrfmt("%d of %d turn%s could not be priced", unpriced, turns, plural(turns));
    return xstrdup(json_is_null(json_get(u, "costUsd")) ? "no turn carries a price" : "every turn priced");
}
static char *duration_or_dash(double ms) { return ms > 0 ? format_duration_ms(ms) : xstrdup("\xE2\x80\x94"); }

/// "2026-08" or "2026-08-03" as the axis names it: "Aug 2026", "Aug 3".
static char *bucket_name(const char *key, bool month) {
    int y = 0, m = 0, d = 1;
    if (!key || sscanf(key, "%d-%d-%d", &y, &m, &d) < 2 || m < 1 || m > 12) return xstrdup(key ? key : "");
    SYSTEMTIME st; memset(&st, 0, sizeof st);
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = (WORD)(d ? d : 1);
    wchar_t buf[64];
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, month ? L"MMM yyyy" : L"MMM d", buf, 64, NULL)) return xstrdup(key);
    return wide_to_utf8(buf);
}
static char *month_name(const char *key) {
    int y = 0, m = 0;
    if (!key || sscanf(key, "%d-%d", &y, &m) < 2 || m < 1 || m > 12) return xstrdup(key ? key : "");
    SYSTEMTIME st; memset(&st, 0, sizeof st);
    st.wYear = (WORD)y; st.wMonth = (WORD)m; st.wDay = 1;
    wchar_t buf[64];
    if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"MMMM yyyy", buf, 64, NULL)) return xstrdup(key);
    return wide_to_utf8(buf);
}
/// What the window is called: the server's month by its own calendar, all time, or the span of its buckets.
static char *window_name(const Json *u) {
    const char *period = json_str(json_get(u, "period"));
    if (str_eq(period, "all")) return xstrdup("all time");
    if (str_eq(period, "month") || str_eq(period, "prev")) return month_name(json_str(json_get(u, "month")));
    const Json *buckets = json_get(u, "buckets");
    size_t n = json_count(buckets);
    if (!n) return xstrdup(PERIODS[g_period].label);
    char *a = bucket_name(json_str(json_get(json_at(buckets, 0), "date")), false);
    char *b = bucket_name(json_str(json_get(json_at(buckets, n - 1), "date")), false);
    char *out = str_eq(a, b) ? xstrdup(a) : xstrfmt("%s \xE2\x80\x93 %s", a, b);
    free(a); free(b);
    return out;
}
static char *model_label(const Json *m) {
    const char *model = json_str_nonempty(json_get(m, "model")), *provider = json_str_nonempty(json_get(m, "provider"));
    return provider ? xstrfmt("%s (%s)", model ? model : "unknown", provider) : xstrdup(model ? model : "unknown");
}

// MARK: - Filters

static bool any_pick(DashboardScreen *s) { for (int f = 0; f < F_COUNT; f++) if (s->pick_count[f]) return true; return false; }
static bool is_picked(DashboardScreen *s, FilterId f, const char *key) {
    for (size_t i = 0; i < s->pick_count[f]; i++) if (str_eq(s->picks[f][i], key)) return true;
    return false;
}
static void picks_clear(DashboardScreen *s, FilterId f) { str_array_free(s->picks[f], s->pick_count[f]); s->picks[f] = NULL; s->pick_count[f] = 0; }
static void pick_add(DashboardScreen *s, FilterId f, const char *key) {
    s->picks[f] = xrealloc(s->picks[f], (s->pick_count[f] + 1) * sizeof *s->picks[f]);
    s->picks[f][s->pick_count[f]++] = xstrdup(key);
}
static void pick_remove(DashboardScreen *s, FilterId f, const char *key) {
    for (size_t i = 0; i < s->pick_count[f]; i++) {
        if (!str_eq(s->picks[f][i], key)) continue;
        free(s->picks[f][i]);
        memmove(&s->picks[f][i], &s->picks[f][i + 1], (s->pick_count[f] - i - 1) * sizeof *s->picks[f]);
        s->pick_count[f]--;
        return;
    }
}
/// A breakdown row's pick: the same one twice comes back out, so a row is a toggle.
static void pick_only(DashboardScreen *s, FilterId f, const char *key) {
    bool same = s->pick_count[f] == 1 && str_eq(s->picks[f][0], key);
    picks_clear(s, f);
    if (!same) pick_add(s, f, key);
}

/// An option's key and the words a picker shows for it.
static const char *option_key(const Json *o) { const char *k = json_str(json_get(o, "key")); return k ? k : ""; }
static char *option_label(FilterId f, const Json *o) {
    if (f == F_MODEL) return model_label(o);
    const char *label = json_str_nonempty(json_get(o, "label"));
    if (f == F_ACTIVITY) return xstrdup(activity_label(option_key(o)));
    if (f == F_PROJECT && json_bool_is(json_get(o, "gone"), true)) return xstrfmt("%s (removed)", label ? label : option_key(o));
    return xstrdup(label ? label : option_key(o));
}
/// What a pick is called: whatever the payload calls it now, else its key.
static char *pick_label(DashboardScreen *s, FilterId f, const char *key) {
    const Json *options = json_get(s->options, FILTERS[f].options);
    for (size_t i = 0; i < json_count(options); i++) if (str_eq(option_key(json_at(options, i)), key)) return option_label(f, json_at(options, i));
    return xstrdup(f == F_ACTIVITY ? activity_label(key) : key);
}
static char *picks_label(DashboardScreen *s, FilterId f) {
    Str out; str_init(&out);
    for (size_t i = 0; i < s->pick_count[f]; i++) {
        char *l = pick_label(s, f, s->picks[f][i]);
        if (i) str_appendz(&out, ", ");
        str_appendz(&out, l); free(l);
    }
    return str_detach(&out);
}

static Json *query_args(DashboardScreen *s) {
    Json *args = json_object();
    json_set_str(args, "period", PERIODS[g_period].id);
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->pick_count[f]) continue;
        if (!FILTERS[f].multi) { json_set_str(args, FILTERS[f].param, s->picks[f][0]); continue; }
        Json *list = json_array();
        for (size_t i = 0; i < s->pick_count[f]; i++) json_array_push(list, json_string(s->picks[f][i]));
        json_object_set(args, FILTERS[f].param, list);
    }
    return args;
}
static char *query_key(DashboardScreen *s) { Json *a = query_args(s); char *k = json_serialize(a, true); json_free(a); return k; }
/// The payload drawn: only the one that answered the current window and picks.
static const Json *shown(DashboardScreen *s) {
    if (!s->data) return NULL;
    char *k = query_key(s);
    bool same = str_eq(k, s->data_key);
    free(k);
    return same ? s->data : NULL;
}

// MARK: - Reading

static void usage_done(void *owner, Request *req) {
    DashboardScreen *s = owner;
    if (req->ok && json_is_object(req->result)) {
        json_free(s->data); free(s->data_key);
        s->data = req->result; req->result = NULL;
        s->data_key = json_serialize(req->args, true);
        if (json_is_object(json_get(s->data, "options"))) { json_free(s->options); s->options = json_clone(json_get(s->data, "options")); }
        if (!any_pick(s)) { char *key = xstrfmt("usage:%s", PERIODS[g_period].id); cache_store(g_store.cache, s->data, key); free(key); }
        set_string(&s->error, NULL);
    } else {
        char *text = req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
        set_string(&s->error, text); free(text);
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}
static void load(DashboardScreen *s) {
    request_cancel(&s->req);
    if (!store_supports("usage_all")) return;
    store_call("usage_all", query_args(s), 60000, s, usage_done, 0, &s->req);
}
/// A new window or pick: straight to the loader, so the page never reads as the old pick's, and ask again.
static void repick(DashboardScreen *s) {
    set_string(&s->error, NULL);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    load(s);
}
static void restore(DashboardScreen *s) {
    char *key = xstrfmt("usage:%s", PERIODS[g_period].id);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    if (!json_is_object(saved)) { json_free(saved); return; }
    s->data = saved; s->data_key = query_key(s);
    if (json_is_object(json_get(saved, "options"))) s->options = json_clone(json_get(saved, "options"));
}

// MARK: - Tiles

/// A row of stat tiles: the label, the figure, and the line under it, four across (two on a narrow pane).
static void tiles(Doc *doc, int x, int w, const char *const *labels, char *const *values, char *const *subs, int n) {
    int cols = w >= px(640) ? 4 : 2, gap = px(8), pad = px(12);
    int tw = (w - gap * (cols - 1)) / cols;
    for (int start = 0; start < n; start += cols) {
        int top = doc->y, bottom = top, boxes[4], count = 0;
        for (int k = 0; k < cols && start + k < n; k++) {
            int i = start + k, tx = x + k * (tw + gap);
            doc->y = top;
            int box = doc_box_begin(doc, tx, tw, pad, theme.raise, theme.line, px(12));
            doc_text(doc, tx + px(14), tw - px(28), labels[i], FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            doc_space(doc, px(4));
            doc_text(doc, tx + px(14), tw - px(28), values[i], FONT_STAT, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            doc_space(doc, px(2));
            doc_text(doc, tx + px(14), tw - px(28), subs[i] ? subs[i] : "", FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
            doc_box_end(doc, box, pad);
            if (doc->y > bottom) bottom = doc->y;
            boxes[count++] = box;
        }
        for (int k = 0; k < count; k++) doc_item(doc, boxes[k])->rc.bottom = bottom;
        doc->y = bottom + (start + cols < n ? gap : 0);
    }
}

// MARK: - Bars

/// One column of a chart; hovering it names its bucket in the chart's heading, as the web's title tooltip does.
typedef struct { int chart; double value, max; char *tip; } BarData;
typedef struct { int chart; char *title; } ChartTitle;
static void bar_free(void *p) { BarData *d = p; free(d->tip); free(d); }
static void title_free(void *p) { ChartTitle *d = p; free(d->title); free(d); }
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    BarData *d = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
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
static bool chart(Doc *doc, int x, int w, const Json *u, int chart_id, const char *metric, const char *title) {
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
        if (num(b, metric) > max) max = num(b, metric);
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
        char *name = bucket_name(date, month);
        int turns = count_of(b, "turns");
        Str tip; str_init(&tip);
        str_appendf(&tip, "%s: ", name);
        if (!turns) str_appendz(&tip, "no usage");
        else {
            char *tok = tokens_text(num(b, "totalTokens")), *cost = cost_text(b);
            str_appendf(&tip, "%s tok \xC2\xB7 %d turn%s", tok, turns, plural(turns));
            if (cost) str_appendf(&tip, " \xC2\xB7 %s", cost);
            free(tok); free(cost);
        }
        BarData *d = xcalloc(1, sizeof *d);
        d->chart = chart_id; d->value = num(b, metric); d->max = max; d->tip = str_detach(&tip);
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

// MARK: - Rings

// The categorical series colours, the only place a hue means which one: four slots and the tail's grey.
enum { SERIES_SLOTS = 4 };
static COLORREF series_color(int i) {
    static const unsigned hex[] = { 0x3987E5, 0xD95926, 0x199E70, 0xC98500, 0x807B71 };
    unsigned h = hex[i < SERIES_SLOTS ? i : SERIES_SLOTS];
    return RGB((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
}
/// shareSeries: the first four rows that spent get a colour each, every other row that spent is summed into Other, and
/// under three slices there is no ring at all (one is a circle, two a ratio the rows already say).
typedef struct { double share[SERIES_SLOTS + 1]; COLORREF color[SERIES_SLOTS + 1]; int count; } Ring;
static bool share_series(const Json *rows, Ring *ring, COLORREF *colors) {
    size_t n = json_count(rows);
    double total = 0;
    for (size_t i = 0; i < n; i++) { colors[i] = CLR_INVALID; total += num(json_at(rows, i), "totalTokens"); }
    memset(ring, 0, sizeof *ring);
    if (total <= 0) return false;
    int spent = 0; double tail = 0;
    for (size_t i = 0; i < n; i++) {
        double t = num(json_at(rows, i), "totalTokens");
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
static void ring_at(Doc *doc, int x, const Ring *ring) {
    int size = px(132);
    Ring *copy = xmalloc(sizeof *copy); *copy = *ring;
    doc_custom(doc, x, size, size, paint_ring, copy, free, 0, 0);
    doc_space(doc, px(6));
    doc_text(doc, x, size, "share of tokens", FONT_CAPTION2, theme.muted, DT_CENTER | DT_SINGLELINE);
}

// MARK: - Tables

enum { COLS_MAX = 8 };
/// One row of a table, painted whole: the cells in their columns, a swatch before the first, and in the project table a
/// bar of its share of the busiest project before the total.
typedef struct {
    int n, x[COLS_MAX], w[COLS_MAX]; UINT align[COLS_MAX];
    char *cell[COLS_MAX]; COLORREF color[COLS_MAX]; FontId font[COLS_MAX];
    char *sub;                 // a second line under the first cell (the session table's repository)
    COLORREF swatch;           // CLR_INVALID for none
    int bar_col; double bar; COLORREF bar_color;   // bar_col -1 for none
    bool head, on;
} RowData;
static void row_free(void *p) { RowData *d = p; for (int i = 0; i < d->n; i++) free(d->cell[i]); free(d->sub); free(d); }
static void paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = it->action && doc->hover >= 0 && doc_item(doc, doc->hover) == it;
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

/// A table being built: its heads, then rows of owned cells, laid out once every width is known.
typedef struct {
    int n; const char *head[COLS_MAX]; UINT align[COLS_MAX]; int fixed[COLS_MAX];   // fixed: a share of the width in %, else 0
    RowData **rows; size_t count;
    FontId head_font; COLORREF head_color; int row_h;
    int flex;                  // the column that takes what the others leave
} Table;
static void table_init(Table *t, int n, const char *const *heads, const UINT *aligns) {
    memset(t, 0, sizeof *t);
    t->n = n;
    for (int i = 0; i < n; i++) { t->head[i] = heads[i]; t->align[i] = aligns[i]; }
    t->head_font = FONT_CAPTION2; t->head_color = theme.muted; t->row_h = px(30);
}
/// A new row with every cell in the body type; the caller sets the cells.
static RowData *table_row(Table *t) {
    RowData *d = xcalloc(1, sizeof *d);
    d->n = t->n; d->swatch = CLR_INVALID; d->bar_col = -1;
    for (int i = 0; i < t->n; i++) { d->align[i] = t->align[i]; d->color[i] = theme.ink; d->font[i] = FONT_FOOTNOTE; }
    t->rows = xrealloc(t->rows, (t->count + 1) * sizeof *t->rows);
    t->rows[t->count++] = d;
    return d;
}
/// Lays the table out at the cursor: each column as wide as its widest cell, the flexible one taking the rest.
static void table_emit(Doc *doc, int x, int w, Table *t, const int *actions, const intptr_t *args) {
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

static void cell(RowData *d, int i, char *text) { free(d->cell[i]); d->cell[i] = text; }
/// The columns every breakdown ends with: sessions, turns, tokens in / out, (total,) time and cost.
static void usage_cells(RowData *d, int from, const Json *u, bool total) {
    cell(d, from, xstrfmt("%d", count_of(u, "sessions")));
    cell(d, from + 1, xstrfmt("%d", count_of(u, "turns")));
    char *in = tokens_text(num(u, "inputTokens")), *out = tokens_text(num(u, "outputTokens"));
    cell(d, from + 2, xstrfmt("%s / %s", in, out)); free(in); free(out);
    int k = from + 3;
    if (total) cell(d, k++, tokens_text(num(u, "totalTokens")));
    cell(d, k, duration_or_dash(num(u, "durationMs")));
    cell(d, k + 1, cost_or_dash(u));
}

/// breakdownCard: the heading, then the ring beside the table on a wide pane and above it on a narrow one.
static void breakdown(Doc *doc, int x, int w, const char *heading, const Ring *ring, bool has_ring, Table *t, const int *actions, const intptr_t *args) {
    doc_space(doc, px(12));
    int pad = px(12), ix = x + px(14), iw = w - px(28);
    int box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    doc_text(doc, ix, iw, heading, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(6));
    int top = doc->y;
    if (has_ring && iw >= px(720)) {
        ring_at(doc, ix, ring);
        int ring_bottom = doc->y, tx = ix + px(132) + px(20);
        doc->y = top;
        table_emit(doc, tx, ix + iw - tx, t, actions, args);
        if (ring_bottom > doc->y) doc->y = ring_bottom;
    } else {
        if (has_ring) { ring_at(doc, ix + (iw - px(132)) / 2, ring); doc_space(doc, px(16)); }
        table_emit(doc, ix, iw, t, actions, args);
    }
    doc_box_end(doc, box, pad);
}

static void project_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u) {
    const Json *rows = json_get(u, "projects");
    size_t n = json_count(rows);
    if (!n) return;
    static const char *const heads[] = { "Project", "Sessions", "Turns", "Tokens in / out", "Total tokens", "Time", "Cost" };
    static const UINT aligns[] = { DT_LEFT, DT_RIGHT, DT_RIGHT, DT_RIGHT, DT_LEFT, DT_RIGHT, DT_RIGHT };
    Table t; table_init(&t, 7, heads, aligns); t.fixed[4] = 26;
    COLORREF *colors = xcalloc(n, sizeof *colors); Ring ring;
    bool has_ring = share_series(rows, &ring, colors);
    double max = 1;
    for (size_t i = 0; i < n; i++) if (num(json_at(rows, i), "totalTokens") > max) max = num(json_at(rows, i), "totalTokens");
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *p = json_at(rows, i);
        RowData *d = table_row(&t);
        const char *label = json_str_nonempty(json_get(p, "label"));
        bool gone = json_bool_is(json_get(p, "gone"), true);
        cell(d, 0, gone ? xstrfmt("%s (removed)", label ? label : "unknown") : xstrdup(label ? label : "unknown"));
        usage_cells(d, 1, p, true);
        // A project that ran nothing, or one no longer in Settings, is greyed out; it still adds up into the totals.
        if (gone || !count_of(p, "turns")) for (int k = 0; k < t.n; k++) d->color[k] = theme.muted;
        // The bar wears its slice's colour, which ties it to the ring; with no ring it is the accent, magnitude alone.
        d->bar_col = 4; d->bar = num(p, "totalTokens") / max; d->bar_color = colors[i] != CLR_INVALID ? colors[i] : theme.accent;
        d->on = is_picked(s, F_PROJECT, option_key(p));
        actions[i] = ACT_ROW; args[i] = (intptr_t)F_PROJECT * ROW_STRIDE + (intptr_t)i;
    }
    breakdown(doc, x, w, "By project", &ring, has_ring, &t, actions, args);
    free(colors); free(actions); free(args);
}

/// The activity, provider and model cards: a swatch on each row for its slice, and the row as a filter.
static void simple_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u, FilterId f) {
    const char *list = f == F_ACTIVITY ? "activities" : f == F_PROVIDER ? "providers" : "models";
    const Json *rows = json_get(u, list);
    size_t n = json_count(rows);
    if (!n) return;
    bool model = f == F_MODEL;
    const char *heads[COLS_MAX]; UINT aligns[COLS_MAX]; int c = 0;
    heads[c] = f == F_ACTIVITY ? "Activity" : model ? "Model" : "Provider"; aligns[c++] = DT_LEFT;
    if (model) { heads[c] = "Provider"; aligns[c++] = DT_LEFT; }
    const char *rest[] = { "Sessions", "Turns", "Tokens in / out", "Time", "Cost" };
    for (int i = 0; i < 5; i++) { heads[c] = rest[i]; aligns[c++] = DT_RIGHT; }
    Table t; table_init(&t, c, heads, aligns);
    COLORREF *colors = xcalloc(n, sizeof *colors); Ring ring;
    bool has_ring = share_series(rows, &ring, colors);
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *r = json_at(rows, i);
        RowData *d = table_row(&t);
        const char *key;
        if (f == F_ACTIVITY) {
            const char *a = json_str_nonempty(json_get(r, "activity"));
            key = a ? a : "unknown";
            cell(d, 0, xstrdup(activity_label(key)));
            if (!a) d->color[0] = theme.muted;
        } else if (f == F_PROVIDER) {
            const char *p = json_str_nonempty(json_get(r, "provider"));
            key = p ? p : "unknown";
            cell(d, 0, xstrdup(key)); d->font[0] = FONT_MONO_SMALL;
            if (!p) d->color[0] = theme.muted;
        } else {
            const char *m = json_str_nonempty(json_get(r, "model")), *p = json_str_nonempty(json_get(r, "provider"));
            key = option_key(r);
            cell(d, 0, xstrdup(m ? m : "unknown")); d->font[0] = FONT_MONO_SMALL;
            cell(d, 1, xstrdup(p ? p : "\xE2\x80\x94")); d->color[1] = theme.muted;
        }
        usage_cells(d, model ? 2 : 1, r, false);
        d->swatch = colors[i];
        d->on = is_picked(s, f, key);
        actions[i] = ACT_ROW; args[i] = (intptr_t)f * ROW_STRIDE + (intptr_t)i;
    }
    breakdown(doc, x, w, f == F_ACTIVITY ? "By activity" : f == F_PROVIDER ? "By provider" : "By model", &ring, has_ring, &t, actions, args);
    free(colors); free(actions); free(args);
}

static void sessions_card(Doc *doc, int x, int w, const Json *u) {
    const Json *rows = json_get(u, "topSessions");
    size_t n = json_count(rows);
    if (!n) return;
    doc_space(doc, px(12));
    int pad = px(12), ix = x + px(12), iw = w - px(24);
    int box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    doc_text(doc, ix, iw, "Most expensive sessions in this selection", FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_text(doc, ix, iw, "Top 10 by known cost. Deleted sessions retain their usage but cannot be reopened.", FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    static const char *const heads[] = { "Session", "Turns", "Tokens", "Cost" };
    static const UINT aligns[] = { DT_LEFT, DT_RIGHT, DT_RIGHT, DT_RIGHT };
    Table t; table_init(&t, 4, heads, aligns);
    t.head_font = FONT_FOOTNOTE_SEMIBOLD; t.head_color = theme.ink; t.row_h = px(52); t.fixed[1] = t.fixed[2] = t.fixed[3] = 15;
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *r = json_at(rows, i);
        RowData *d = table_row(&t);
        const char *label = json_str_nonempty(json_get(r, "label"));
        cell(d, 0, xstrdup(label ? label : option_key(r))); d->color[0] = theme.accent; d->font[0] = FONT_SUBHEADLINE;
        const char *repo = json_str(json_get(r, "repo"));
        d->sub = xstrdup(repo ? repo : "");
        cell(d, 1, xstrfmt("%d", count_of(r, "turns")));
        cell(d, 2, tokens_text(num(r, "totalTokens")));
        cell(d, 3, cost_or_dash(r));
        for (int k = 1; k < 4; k++) d->font[k] = FONT_SUBHEADLINE;
        actions[i] = ACT_OPEN_SESSION; args[i] = (intptr_t)i;
    }
    table_emit(doc, ix, iw, &t, actions, args);
    doc_box_end(doc, box, pad);
    free(actions); free(args);
}

/// homeInsights: the averages, how the window compares with the one before, cost per bucket and the costliest sessions.
static void insights(Doc *doc, int x, int w, const Json *u) {
    const Json *i = json_get(u, "insights");
    char *values[4], *subs[4];
    double v;
    values[0] = json_num(json_get(i, "costPerSession"), &v) ? xstrfmt("$%.2f%s", v, count_of(u, "unpricedTurns") ? "+" : "") : xstrdup("\xE2\x80\x94");
    values[1] = json_num(json_get(i, "costPerTurn"), &v) ? xstrfmt("$%.2f%s", v, count_of(u, "unpricedTurns") ? "+" : "") : xstrdup("\xE2\x80\x94");
    values[2] = json_num(json_get(i, "averageDurationMs"), &v) ? format_duration_ms(v) : xstrdup("\xE2\x80\x94");
    int turns = count_of(u, "turns"), priced = turns - count_of(u, "unpricedTurns");
    values[3] = xstrfmt("%d%%", turns ? (int)(priced * 100.0 / turns + 0.5) : 0);
    subs[0] = xstrdup("within this selection"); subs[1] = xstrdup("within this selection");
    subs[2] = xstrfmt("%d turns with timing", count_of(i, "timedTurns"));
    subs[3] = xstrfmt("%d of %d turns priced", priced, turns);
    static const char *const labels[] = { "Cost / session", "Cost / turn", "Average turn duration", "Pricing coverage" };
    doc_space(doc, px(12));
    tiles(doc, x, w, labels, values, subs, 4);
    for (int k = 0; k < 4; k++) { free(values[k]); free(subs[k]); }

    const Json *prev = json_get(u, "comparison");
    if (json_is_object(prev)) {
        double from = num(prev, "from"), to = num(prev, "to");
        char *a = format_date_abbrev((time_t)(from / 1000)), *b = format_date_abbrev((time_t)((to - 1) / 1000));
        Str line; str_init(&line);
        str_appendf(&line, "Compared with %s \xE2\x80\x93 %s%s: ", a, b, json_bool_is(json_get(prev, "partial"), true) ? " (matching elapsed time)" : "");
        static const char *const keys[] = { "costUsd", "totalTokens", "sessions" }, *const names[] = { "Cost", "Tokens", "Sessions" };
        for (int k = 0; k < 3; k++) {
            double old, now;
            if (k) str_appendz(&line, " \xC2\xB7 ");
            if (!json_num(json_get(prev, keys[k]), &old) || !json_num(json_get(u, keys[k]), &now)) str_appendf(&line, "%s: unavailable", names[k]);
            else if (!old) str_appendf(&line, "%s: %s", names[k], now ? "new usage (previously zero)" : "unchanged");
            else { double pct = (now - old) / old * 100; str_appendf(&line, "%s: %s%.1f%%", names[k], pct > 0 ? "+" : "", pct); }
        }
        if (count_of(u, "unpricedTurns") || count_of(prev, "unpricedTurns")) str_appendz(&line, " \xC2\xB7 cost comparison is partial");
        doc_space(doc, px(12));
        doc_text(doc, x, w, line.data, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
        str_free(&line); free(a); free(b);
    }
    bool month = str_eq(json_str(json_get(u, "unit")), "month");
    if (!chart(doc, x, w, u, 1, "costUsd", month ? "Cost per month" : "Cost per day")) {
        doc_space(doc, px(12));
        doc_text(doc, x, w, "No priced spend to plot in this selection.", FONT_CAPTION, theme.muted, DT_LEFT);
    }
    sessions_card(doc, x, w, u);
}

// MARK: - Screen

static char *filter_button_text(DashboardScreen *s, FilterId f) {
    size_t n = s->pick_count[f];
    if (!n) return xstrfmt("All %s \xE2\x96\xBE", FILTERS[f].plural);
    if (FILTERS[f].multi && n > 1) return xstrfmt("%zu %s \xE2\x96\xBE", n, FILTERS[f].plural);
    char *l = picks_label(s, f), *out = xstrfmt("%s \xE2\x96\xBE", l);
    free(l);
    return out;
}

static void dashboard_layout(Screen *base, Doc *doc) {
    DashboardScreen *s = (DashboardScreen *)base;
    int x = 0, w = doc->width;
    doc_space(doc, px(14));
    if (!store_supports("usage_all")) {
        bool listed = false;
        for (size_t i = 0; i < g_store.route_count; i++) if (str_eq(g_store.routes[i].path, "/usage/all") || str_eq(g_store.routes[i].path, "usage/all")) listed = true;
        const char *permission = g_store.has_device && g_store.device.permission ? g_store.device.permission : "unknown";
        char *why = listed
            ? xstrfmt("The dashboard reads every project's spend, which needs an Admin token, and this device's token is %s. Create an Admin token on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", permission)
            : xstrdup("This server does not offer the usage ledger (GET /usage/all) on its client API. Update the server to see the dashboard here.");
        doc_text(doc, x, w, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        free(why);
        doc_space(doc, px(12));
        return;
    }
    // The pickers, as the dashboard's selects, and Clear filters.
    char *texts[F_COUNT];
    ButtonSpec buttons[F_COUNT + 1];
    for (int f = 0; f < F_COUNT; f++) {
        texts[f] = filter_button_text(s, (FilterId)f);
        ButtonSpec b = { 0, texts[f], BUTTON_BORDERED, ACT_FILTER, f, s->options != NULL };
        buttons[f] = b;
    }
    ButtonSpec clear = { 0, "Clear filters", BUTTON_BORDERED, ACT_CLEAR, 0, any_pick(s) };
    buttons[F_COUNT] = clear;
    doc_button_row(doc, x, w, buttons, F_COUNT + 1);
    for (int f = 0; f < F_COUNT; f++) free(texts[f]);
    doc_space(doc, px(14));

    const Json *u = shown(s);
    if (s->error && !u) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); return; }
    if (!u) { doc_text(doc, x, w, "Loading the usage ledger\xE2\x80\xA6", FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_SINGLELINE); doc_space(doc, px(12)); return; }
    if (s->error) { doc_notice(doc, x, w, s->error); doc_space(doc, px(10)); }
    if (!count_of(u, "turns")) {
        // An empty window still lists the projects, at zero: "which projects ran nothing?" is the question left.
        char *over = window_name(u);
        char *line = xstrfmt("No usage recorded in %s%s.", over, any_pick(s) ? " for these filters" : "");
        doc_space(doc, px(18));
        doc_text(doc, x, w, line, FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_WORDBREAK);
        doc_space(doc, px(18));
        free(line); free(over);
        insights(doc, x, w, u);
        project_card(s, doc, x, w, u);
        doc_space(doc, px(16));
        return;
    }
    static const char *const labels[] = { "Cost", "Tokens", "Sessions", "Agent time" };
    char *values[4], *subs[4];
    values[0] = cost_or_dash(u); subs[0] = cost_note(u);
    values[1] = tokens_text(num(u, "totalTokens"));
    { char *in = tokens_text(num(u, "inputTokens")), *out = tokens_text(num(u, "outputTokens")); subs[1] = xstrfmt("%s in \xC2\xB7 %s out", in, out); free(in); free(out); }
    values[2] = xstrfmt("%d", count_of(u, "sessions"));
    subs[2] = xstrfmt("%d turn%s", count_of(u, "turns"), plural(count_of(u, "turns")));
    values[3] = duration_or_dash(num(u, "durationMs")); subs[3] = xstrdup("summed over every turn");
    tiles(doc, x, w, labels, values, subs, 4);
    for (int k = 0; k < 4; k++) { free(values[k]); free(subs[k]); }
    bool month = str_eq(json_str(json_get(u, "unit")), "month");
    chart(doc, x, w, u, 0, "totalTokens", month ? "Tokens per month" : "Tokens per day");
    insights(doc, x, w, u);
    project_card(s, doc, x, w, u);
    simple_card(s, doc, x, w, u, F_ACTIVITY);
    simple_card(s, doc, x, w, u, F_PROVIDER);
    simple_card(s, doc, x, w, u, F_MODEL);
    doc_space(doc, px(16));
}

static void dashboard_header(Screen *base, HeaderInfo *info) {
    DashboardScreen *s = (DashboardScreen *)base;
    snprintf(info->title, sizeof info->title, "\xF0\x9F\x93\x8A Dashboard");
    const Json *u = shown(s);
    if (u) {
        // A filter is said in the subtitle as well as in its picker: these totals are a slice, and this line says which.
        Str sub; str_init(&sub);
        char *over = window_name(u);
        str_appendz(&sub, over); free(over);
        bool picked = false;
        for (int f = 0; f < F_COUNT; f++) {
            if (!s->pick_count[f]) continue;
            char *l = picks_label(s, (FilterId)f);
            str_appendf(&sub, " \xC2\xB7 only %s", l); free(l);
            picked = true;
        }
        if (!picked) {
            int active = 0;
            const Json *projects = json_get(u, "projects");
            for (size_t i = 0; i < json_count(projects); i++) if (count_of(json_at(projects, i), "turns")) active++;
            str_appendf(&sub, " \xC2\xB7 %d project%s with usage", active, plural(active));
        }
        str_appendf(&sub, " \xC2\xB7 %d turn%s", count_of(u, "turns"), plural(count_of(u, "turns")));
        snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
        str_free(&sub);
    }
    if (!store_supports("usage_all")) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", PERIODS[g_period].label);
    b->glyph = 0xE787; b->action = ACT_PERIOD; b->enabled = true; b->tip = "The window every number on this page is over";
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = !s->req; r->tip = "Read the usage ledger again";
}

static void pick_period(DashboardScreen *s, POINT pt) {
    HMENU menu = CreatePopupMenu();
    for (int i = 0; i < PERIOD_COUNT; i++) {
        wchar_t *w = utf8_to_wide(PERIODS[i].label);
        AppendMenuW(menu, MF_STRING | (i == g_period ? MF_CHECKED : 0), (UINT_PTR)(1 + i), w);
        free(w);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen < 1 || chosen - 1 == g_period) return;
    g_period = chosen - 1;
    repick(s);
}
/// One picker's menu: All, then the window's options. The project and model pickers tick several; the others take one.
static void pick_filter(DashboardScreen *s, FilterId f, POINT pt) {
    const Json *options = json_get(s->options, FILTERS[f].options);
    size_t n = json_count(options), listed = n < MENU_CAP ? n : MENU_CAP;
    HMENU menu = CreatePopupMenu();
    char *all = xstrfmt("All %s", FILTERS[f].plural); wchar_t *wall = utf8_to_wide(all);
    AppendMenuW(menu, MF_STRING | (s->pick_count[f] ? 0 : MF_CHECKED), 1, wall);
    free(all); free(wall);
    if (n) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    for (size_t i = 0; i < listed; i++) {
        const Json *o = json_at(options, i);
        char *label = option_label(f, o); wchar_t *wl = utf8_to_wide(label);
        AppendMenuW(menu, MF_STRING | (is_picked(s, f, option_key(o)) ? MF_CHECKED : 0), (UINT_PTR)(2 + i), wl);
        free(label); free(wl);
    }
    if (listed < n) {
        char *more = xstrfmt("%zu more %s; a shorter window lists them", n - listed, FILTERS[f].plural); wchar_t *wm = utf8_to_wide(more);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, wm);
        free(more); free(wm);
    }
    if (!n) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"No usage in this period");
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) { if (!s->pick_count[f]) return; picks_clear(s, f); }
    else if (chosen >= 2 && (size_t)(chosen - 2) < listed) {
        const char *key = option_key(json_at(options, (size_t)(chosen - 2)));
        if (FILTERS[f].multi) { if (is_picked(s, f, key)) pick_remove(s, f, key); else pick_add(s, f, key); }
        else { if (is_picked(s, f, key)) return; picks_clear(s, f); pick_add(s, f, key); }
    } else return;
    repick(s);
}
static void open_top_session(DashboardScreen *s, size_t index) {
    const Json *u = shown(s);
    const Json *r = json_at(json_get(u, "topSessions"), index);
    const char *id = json_str_nonempty(json_get(r, "key"));
    if (!id) return;
    // The ledger knows the id, title and project; the conversation reads the rest itself.
    Json *raw = json_object();
    json_set_str(raw, "id", id); json_set_str(raw, "status", "idle");
    json_set_str(raw, "repo", json_str(json_get(r, "repo")));
    const char *label = json_str_nonempty(json_get(r, "label"));
    if (label && !str_eq(label, id)) json_set_str(raw, "title", label);
    Session ses;
    if (session_parse(raw, &ses)) { app_push_detail(conversation_screen_new(&ses)); session_free(&ses); }
    json_free(raw);
}
static void dashboard_action(Screen *base, int action, intptr_t arg, POINT pt) {
    DashboardScreen *s = (DashboardScreen *)base;
    switch (action) {
    case ACT_PERIOD: pick_period(s, pt); break;
    case ACT_REFRESH: load(s); pane_header_changed(base->pane); break;
    case ACT_FILTER: if (arg >= 0 && arg < F_COUNT) pick_filter(s, (FilterId)arg, pt); break;
    case ACT_CLEAR: for (int f = 0; f < F_COUNT; f++) picks_clear(s, (FilterId)f); repick(s); break;
    case ACT_OPEN_SESSION: open_top_session(s, (size_t)arg); break;
    case ACT_ROW: {
        const Json *u = shown(s);
        FilterId f = (FilterId)(arg / ROW_STRIDE);
        size_t i = (size_t)(arg % ROW_STRIDE);
        if (!u || f < 0 || f >= F_COUNT) break;
        const char *list = f == F_PROJECT ? "projects" : f == F_ACTIVITY ? "activities" : f == F_PROVIDER ? "providers" : "models";
        const Json *r = json_at(json_get(u, list), i);
        if (json_is_null(r)) break;
        const char *key = f == F_ACTIVITY ? json_str_nonempty(json_get(r, "activity")) : f == F_PROVIDER ? json_str_nonempty(json_get(r, "provider")) : option_key(r);
        pick_only(s, f, key ? key : "unknown");
        repick(s);
        break;
    }
    }
}

static void dashboard_timer(Screen *base, UINT id) {
    DashboardScreen *s = (DashboardScreen *)base;
    if (!poller_fired(&s->poller, id)) return;
    if (s->req) poller_finished(&s->poller, false, -1); else load(s);
}
static void dashboard_visible(Screen *base, bool shown_now) {
    DashboardScreen *s = (DashboardScreen *)base;
    if (shown_now) poller_start(&s->poller, base->pane, TIMER_POLL, 60000);
    else { poller_stop(&s->poller); request_cancel(&s->req); }
}
static void dashboard_refresh(Screen *base) { DashboardScreen *s = (DashboardScreen *)base; load(s); pane_header_changed(base->pane); }
static void dashboard_activated(Screen *base, bool active) { if (active) { DashboardScreen *s = (DashboardScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 60000); } }
static void dashboard_destroy(Screen *base) {
    DashboardScreen *s = (DashboardScreen *)base;
    poller_stop(&s->poller);
    request_cancel(&s->req);
    json_free(s->data); json_free(s->options); free(s->data_key); free(s->error);
    for (int f = 0; f < F_COUNT; f++) picks_clear(s, (FilterId)f);
    screen_release(base);
}

static const ScreenVTable dashboard_vt = {
    .destroy = dashboard_destroy, .layout = dashboard_layout, .header = dashboard_header, .action = dashboard_action,
    .timer = dashboard_timer, .visible = dashboard_visible, .refresh = dashboard_refresh, .activated = dashboard_activated,
};
Screen *dashboard_screen_new(void) {
    DashboardScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &dashboard_vt; s->base.id = xstrdup("dashboard");
    restore(s);
    return &s->base;
}
