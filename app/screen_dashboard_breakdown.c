#include "screen_dashboard_internal.h"

static char *cost_value(const Json *u, const char *key);
static char *month_name(const char *key);
static void breakdown(Doc *doc, int x, int w, const char *heading, const Ring *ring, bool has_ring, Table *t, const int *actions, const intptr_t *args);
static void sessions_card(Doc *doc, int x, int w, const Json *u);

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

const char *screen_dashboard_activity_label(const char *id) {
    if (str_empty(id) || str_eq(id, "unknown")) return "Unattributed";
    for (size_t i = 0; i < sizeof ACTIVITIES / sizeof *ACTIVITIES; i++) if (str_eq(ACTIVITIES[i].id, id)) return ACTIVITIES[i].label;
    return id;
}

// MARK: - Numbers as the dashboard words them

double screen_dashboard_num(const Json *u, const char *key) { return json_num_or(json_get(u, key), 0); }

int screen_dashboard_count_of(const Json *u, const char *key) { return json_int_or(json_get(u, key), 0); }

const char *screen_dashboard_plural(int n) { return n == 1 ? "" : "s"; }

/// fmtTokens: "21599.7M", "80.5M", "93.0k".
char *screen_dashboard_tokens_text(double n) {
    if (n >= 1e6) return xstrfmt("%.1fM", n / 1e6);
    if (n >= 1000) return xstrfmt("%.1fk", n / 1000);
    return xstrfmt("%.0f", n);
}

/// fmtCost: one price, and a `+` when some turns carry none, so the total is a floor. NULL when nothing was priced.
static char *cost_value(const Json *u, const char *key) {
    double usd;
    if (!json_num(json_get(u, key), &usd)) return NULL;
    return xstrfmt("$%.2f%s", usd, screen_dashboard_count_of(u, "unpricedTurns") ? "+" : "");
}

char *screen_dashboard_cost_text(const Json *u) { return cost_value(u, "costUsd"); }

char *screen_dashboard_cost_or_dash(const Json *u) { char *c = screen_dashboard_cost_text(u); return c ? c : xstrdup("\xE2\x80\x94"); }

char *screen_dashboard_cost_note(const Json *u) {
    int unpriced = screen_dashboard_count_of(u, "unpricedTurns"), turns = screen_dashboard_count_of(u, "turns");
    if (unpriced) return xstrfmt("%d of %d turn%s could not be priced", unpriced, turns, screen_dashboard_plural(turns));
    return xstrdup(json_is_null(json_get(u, "costUsd")) ? "no turn carries a price" : "every turn priced");
}

char *screen_dashboard_duration_or_dash(double ms) { return ms > 0 ? format_duration_ms(ms) : xstrdup("\xE2\x80\x94"); }

/// "2026-08" or "2026-08-03" as the axis names it: "Aug 2026", "Aug 3".
char *screen_dashboard_bucket_name(const char *key, bool month) {
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
char *screen_dashboard_window_name(const Json *u, const UsageWindow *window) {
    const char *period = json_str(json_get(u, "period"));
    if (str_eq(period, "all")) return xstrdup("all time");
    if (str_eq(period, "month") || str_eq(period, "prev")) return month_name(json_str(json_get(u, "month")));
    const Json *buckets = json_get(u, "buckets");
    size_t n = json_count(buckets);
    if (!n || window->period == USAGE_CUSTOM) return usage_window_label(window);
    char *a = screen_dashboard_bucket_name(json_str(json_get(json_at(buckets, 0), "date")), false);
    char *b = screen_dashboard_bucket_name(json_str(json_get(json_at(buckets, n - 1), "date")), false);
    char *out = str_eq(a, b) ? xstrdup(a) : xstrfmt("%s \xE2\x80\x93 %s", a, b);
    free(a); free(b);
    return out;
}

char *screen_dashboard_model_label(const Json *m) {
    const char *model = json_str_nonempty(json_get(m, "model")), *provider = json_str_nonempty(json_get(m, "provider"));
    return provider ? xstrfmt("%s (%s)", model ? model : "unknown", provider) : xstrdup(model ? model : "unknown");
}

/// A row of stat tiles: the label, the figure, and the line under it, four across (two on a narrow pane).
void screen_dashboard_tiles(Doc *doc, int x, int w, const char *const *labels, char *const *values, char *const *subs, int n) {
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

/// breakdownCard: the heading, then the ring beside the table on a wide pane and above it on a narrow one.
static void breakdown(Doc *doc, int x, int w, const char *heading, const Ring *ring, bool has_ring, Table *t, const int *actions, const intptr_t *args) {
    doc_space(doc, px(12));
    int pad = px(12), ix = x + px(14), iw = w - px(28);
    int box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(12));
    doc_text(doc, ix, iw, heading, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(6));
    int top = doc->y;
    if (has_ring && iw >= px(720)) {
        screen_dashboard_ring_at(doc, ix, ring);
        int ring_bottom = doc->y, tx = ix + px(132) + px(20);
        doc->y = top;
        screen_dashboard_table_emit(doc, tx, ix + iw - tx, t, actions, args);
        if (ring_bottom > doc->y) doc->y = ring_bottom;
    } else {
        if (has_ring) { screen_dashboard_ring_at(doc, ix + (iw - px(132)) / 2, ring); doc_space(doc, px(16)); }
        screen_dashboard_table_emit(doc, ix, iw, t, actions, args);
    }
    doc_box_end(doc, box, pad);
}

void screen_dashboard_project_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u) {
    const Json *rows = json_get(u, "projects");
    size_t n = json_count(rows);
    if (!n) return;
    static const char *const heads[] = { "Project", "Sessions", "Turns", "Tokens in / out", "Total tokens", "Time", "Cost" };
    static const UINT aligns[] = { DT_LEFT, DT_RIGHT, DT_RIGHT, DT_RIGHT, DT_LEFT, DT_RIGHT, DT_RIGHT };
    Table t; screen_dashboard_table_init(&t, 7, heads, aligns); t.fixed[4] = 26;
    COLORREF *colors = xcalloc(n, sizeof *colors); Ring ring;
    bool has_ring = screen_dashboard_share_series(rows, &ring, colors);
    double max = 1;
    for (size_t i = 0; i < n; i++) if (screen_dashboard_num(json_at(rows, i), "totalTokens") > max) max = screen_dashboard_num(json_at(rows, i), "totalTokens");
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *p = json_at(rows, i);
        RowData *d = screen_dashboard_table_row(&t);
        const char *label = json_str_nonempty(json_get(p, "label"));
        bool gone = json_bool_is(json_get(p, "gone"), true);
        screen_dashboard_cell(d, 0, gone ? xstrfmt("%s (removed)", label ? label : "unknown") : xstrdup(label ? label : "unknown"));
        screen_dashboard_usage_cells(d, 1, p, true);
        // A project that ran nothing, or one no longer in Settings, is greyed out; it still adds up into the totals.
        if (gone || !screen_dashboard_count_of(p, "turns")) for (int k = 0; k < t.n; k++) d->color[k] = theme.muted;
        // The bar wears its slice's colour, which ties it to the ring; with no ring it is the accent, magnitude alone.
        d->bar_col = 4; d->bar = screen_dashboard_num(p, "totalTokens") / max; d->bar_color = colors[i] != CLR_INVALID ? colors[i] : theme.accent;
        d->on = screen_dashboard_is_picked(s, F_PROJECT, screen_dashboard_option_key(p));
        actions[i] = ACT_ROW; args[i] = (intptr_t)F_PROJECT * ROW_STRIDE + (intptr_t)i;
    }
    breakdown(doc, x, w, "By project", &ring, has_ring, &t, actions, args);
    free(colors); free(actions); free(args);
}

/// The activity, provider and model cards: a swatch on each row for its slice, and the row as a filter.
void screen_dashboard_simple_card(DashboardScreen *s, Doc *doc, int x, int w, const Json *u, FilterId f) {
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
    Table t; screen_dashboard_table_init(&t, c, heads, aligns);
    COLORREF *colors = xcalloc(n, sizeof *colors); Ring ring;
    bool has_ring = screen_dashboard_share_series(rows, &ring, colors);
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *r = json_at(rows, i);
        RowData *d = screen_dashboard_table_row(&t);
        const char *key;
        if (f == F_ACTIVITY) {
            const char *a = json_str_nonempty(json_get(r, "activity"));
            key = a ? a : "unknown";
            screen_dashboard_cell(d, 0, xstrdup(screen_dashboard_activity_label(key)));
            if (!a) d->color[0] = theme.muted;
        } else if (f == F_PROVIDER) {
            const char *p = json_str_nonempty(json_get(r, "provider"));
            key = p ? p : "unknown";
            screen_dashboard_cell(d, 0, xstrdup(key)); d->font[0] = FONT_MONO_SMALL;
            if (!p) d->color[0] = theme.muted;
        } else {
            const char *m = json_str_nonempty(json_get(r, "model")), *p = json_str_nonempty(json_get(r, "provider"));
            key = screen_dashboard_option_key(r);
            screen_dashboard_cell(d, 0, xstrdup(m ? m : "unknown")); d->font[0] = FONT_MONO_SMALL;
            screen_dashboard_cell(d, 1, xstrdup(p ? p : "\xE2\x80\x94")); d->color[1] = theme.muted;
        }
        screen_dashboard_usage_cells(d, model ? 2 : 1, r, false);
        d->swatch = colors[i];
        d->on = screen_dashboard_is_picked(s, f, key);
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
    Table t; screen_dashboard_table_init(&t, 4, heads, aligns);
    t.head_font = FONT_FOOTNOTE_SEMIBOLD; t.head_color = theme.ink; t.row_h = px(52); t.fixed[1] = t.fixed[2] = t.fixed[3] = 15;
    int *actions = xcalloc(n, sizeof *actions); intptr_t *args = xcalloc(n, sizeof *args);
    for (size_t i = 0; i < n; i++) {
        const Json *r = json_at(rows, i);
        RowData *d = screen_dashboard_table_row(&t);
        const char *label = json_str_nonempty(json_get(r, "label"));
        screen_dashboard_cell(d, 0, xstrdup(label ? label : screen_dashboard_option_key(r))); d->color[0] = theme.accent; d->font[0] = FONT_SUBHEADLINE;
        const char *repo = json_str(json_get(r, "repo"));
        d->sub = xstrdup(repo ? repo : "");
        screen_dashboard_cell(d, 1, xstrfmt("%d", screen_dashboard_count_of(r, "turns")));
        screen_dashboard_cell(d, 2, screen_dashboard_tokens_text(screen_dashboard_num(r, "totalTokens")));
        screen_dashboard_cell(d, 3, screen_dashboard_cost_or_dash(r));
        for (int k = 1; k < 4; k++) d->font[k] = FONT_SUBHEADLINE;
        actions[i] = ACT_OPEN_SESSION; args[i] = (intptr_t)i;
    }
    screen_dashboard_table_emit(doc, ix, iw, &t, actions, args);
    doc_box_end(doc, box, pad);
    free(actions); free(args);
}

/// homeInsights: the averages, how the window compares with the one before, cost per bucket and the costliest sessions.
void screen_dashboard_insights(Doc *doc, int x, int w, const Json *u) {
    const Json *i = json_get(u, "insights");
    char *values[4], *subs[4];
    double v;
    values[0] = json_num(json_get(i, "costPerSession"), &v) ? xstrfmt("$%.2f%s", v, screen_dashboard_count_of(u, "unpricedTurns") ? "+" : "") : xstrdup("\xE2\x80\x94");
    values[1] = json_num(json_get(i, "costPerTurn"), &v) ? xstrfmt("$%.2f%s", v, screen_dashboard_count_of(u, "unpricedTurns") ? "+" : "") : xstrdup("\xE2\x80\x94");
    values[2] = json_num(json_get(i, "averageDurationMs"), &v) ? format_duration_ms(v) : xstrdup("\xE2\x80\x94");
    UsagePricing pricing = usage_pricing(u);
    values[3] = xstrfmt("%d%%", pricing.percent);
    subs[0] = xstrdup("within this selection"); subs[1] = xstrdup("within this selection");
    subs[2] = xstrfmt("%d turns with timing", screen_dashboard_count_of(i, "timedTurns"));
    subs[3] = xstrfmt("%d of %d turns priced", pricing.priced_turns, pricing.turns);
    static const char *const labels[] = { "Cost / session", "Cost / turn", "Average turn duration", "Pricing coverage" };
    doc_space(doc, px(12));
    screen_dashboard_tiles(doc, x, w, labels, values, subs, 4);
    for (int k = 0; k < 4; k++) { free(values[k]); free(subs[k]); }

    UsageComparison comparison = usage_comparison(u);
    if (comparison.present) {
        char *a = format_date_abbrev((time_t)(comparison.from_ms / 1000)), *b = format_date_abbrev((time_t)((comparison.to_ms - 1) / 1000));
        Str line; str_init(&line);
        str_appendf(&line, "Compared with %s \xE2\x80\x93 %s%s: ", a, b, comparison.partial_elapsed ? " (matching elapsed time)" : "");
        static const char *const names[] = { "Cost", "Tokens", "Sessions" };
        for (int k = 0; k < USAGE_METRIC_COUNT; k++) {
            UsageChange delta = comparison.changes[k];
            if (k) str_appendz(&line, " \xC2\xB7 ");
            if (delta.kind == USAGE_CHANGE_UNAVAILABLE) str_appendf(&line, "%s: unavailable", names[k]);
            else if (delta.kind != USAGE_CHANGE_PERCENT) str_appendf(&line, "%s: %s", names[k], delta.kind == USAGE_CHANGE_NEW ? "new usage (previously zero)" : "unchanged");
            else str_appendf(&line, "%s: %s%.1f%%", names[k], delta.percent > 0 ? "+" : "", delta.percent);
        }
        if (comparison.partial_cost) str_appendz(&line, " \xC2\xB7 cost comparison is partial");
        doc_space(doc, px(12));
        doc_text(doc, x, w, line.data, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
        str_free(&line); free(a); free(b);
    }
    bool month = str_eq(json_str(json_get(u, "unit")), "month");
    if (!screen_dashboard_chart(doc, x, w, u, 1, "costUsd", month ? "Cost per month" : "Cost per day")) {
        doc_space(doc, px(12));
        doc_text(doc, x, w, "No priced spend to plot in this selection.", FONT_CAPTION, theme.muted, DT_LEFT);
    }
    sessions_card(doc, x, w, u);
}
