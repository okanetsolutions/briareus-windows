#include "screen_panel_internal.h"

typedef struct { size_t count; COLORREF *colors; double *pct; } BarData;

static COLORREF context_color(const char *name);
static void bar_free(void *p);
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_checkbox(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_swatch(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void usage_row(Doc *doc, int w, const char *label, const char *value);
static bool num_at(const Json *object, const char *key, double *out);
static const Json *usage_of(const Json *raw);
static bool context_size(const Json *raw, double *used, double *window);
static bool can_op(const char *operation);
static bool auto_compact_shown(const Json *raw);
static bool instructions_shown(const Json *raw);
static bool compact_shown(const Json *raw);
static bool usage_rows_present(const Json *raw);
static char *usage_time(const Json *value);

// MARK: - Context usage

/// The bar's colours, keyed on the category names claude's /context report uses (briareus_tokens.h), dark or light as the
/// app is: deferred and buffer rows a dim step above free space, which is the track's own line colour.
static COLORREF context_color(const char *name) {
    char *n = str_fold(name ? name : "");
    unsigned h = briareus_context_color(theme.dark ? &BRIAREUS_CONTEXT_DARK : &BRIAREUS_CONTEXT_LIGHT, n);
    free(n);
    return RGB((h >> 16) & 0xFF, (h >> 8) & 0xFF, h & 0xFF);
}

static void bar_free(void *p) { BarData *d = p; free(d->colors); free(d->pct); free(d); }

/// `flex h-1.5 overflow-hidden rounded-full bg-sunken`, one segment per category.
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    BarData *d = it->data;
    int h = rc->bottom - rc->top, w = rc->right - rc->left;
    canvas_clip_round(cv, rc, h / 2);
    fill_rect(cv, rc, theme.sunken);
    double x = rc->left;
    for (size_t i = 0; i < d->count; i++) {
        double pct = d->pct[i] < 0 ? 0 : d->pct[i] > 100 ? 100 : d->pct[i];
        double next = x + w * pct / 100;
        RECT seg = { (int)(x + 0.5), rc->top, (int)(next + 0.5), rc->bottom };
        if (seg.right > seg.left) fill_rect(cv, &seg, d->colors[i]);
        x = next;
    }
    canvas_unclip(cv);
}

/// `<input type="checkbox" class="accent-accent">`
static void paint_checkbox(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    bool on = it->arg != 0;
    fill_round_rect(cv, rc, px(3), on ? theme.accent : theme.field, on ? theme.accent : theme.line_strong);
    if (on) draw_check_mark(cv, rc, theme.on_accent);
}

static void paint_swatch(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    COLORREF color = (COLORREF)it->arg;
    fill_round_rect(cv, rc, px(3), color, color);
}

/// `flex justify-between text-[12px] text-muted`: the label, and the value in ink at the right.
static void usage_row(Doc *doc, int w, const char *label, const char *value) {
    int y = doc->y, h = px(18);
    int vw = text_width(doc->cv, value, FONT_CAPTION) + 2;
    RECT vr = { w - vw, y, w, y + h }, lr = { 0, y, w - vw - px(6), y + h };
    doc_text_at(doc, &lr, label, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_text_at(doc, &vr, value, FONT_CAPTION, theme.ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    doc->y = y + h + px(2);
}

static bool num_at(const Json *object, const char *key, double *out) { return json_num(json_get(object, key), out); }

/// What the session spent with everything it ordered, or its own figures from a server without `usage`.
static const Json *usage_of(const Json *raw) { const Json *u = json_get(raw, "usage"); return json_is_object(u) ? u : raw; }

static bool context_size(const Json *raw, double *used, double *window) {
    const Json *cu = json_get(raw, "contextUsage");
    if (!num_at(cu, "window", window) && !num_at(raw, "contextWindow", window)) *window = 0;
    return num_at(cu, "tokens", used) || num_at(raw, "contextTokens", used);
}

static bool can_op(const char *operation) { return store_supports(operation) && store_can_manage(); }

static bool auto_compact_shown(const Json *raw) { double at; return num_at(raw, "autoCompactAt", &at) && at > 0 && can_op("rename"); }

static bool instructions_shown(const Json *raw) {
    return (json_bool_is(json_get(raw, "compactTakesInstructions"), true) || json_str_nonempty(json_get(raw, "compactInstructions"))) && can_op("rename");
}

static bool compact_shown(const Json *raw) {
    return (json_bool_is(json_get(raw, "canCompact"), true) || json_bool_is(json_get(raw, "compacting"), true)) && can_op("compact");
}

static bool usage_rows_present(const Json *raw) {
    const Json *u = usage_of(raw);
    static const char *const keys[] = { "inputTokens", "outputTokens", "durationMs", "costUsd" };
    double v;
    for (size_t i = 0; i < 4; i++) if (num_at(u, keys[i], &v)) return true;
    return false;
}

bool session_panel_wanted(const Session *session) {
    if (session_pull_number(session) && session_repo(session) && store_supports("pull")) return true;
    const Json *raw = session->raw;
    double used, window;
    return context_size(raw, &used, &window) || usage_rows_present(raw) || compact_shown(raw) || auto_compact_shown(raw) || instructions_shown(raw);
}

/// An ISO time as the transcript words it ("2:13 PM", or with the day when not today).
static char *usage_time(const Json *value) {
    time_t when;
    if (!board_date_parse(json_str(value), &when)) return NULL;
    return format_event_time(when);
}

/// The dashboard's `usageSection`: how much of the model's window is used, as a bar split into claude's /context
/// categories when the server has them and used-vs-free otherwise, then what the session consumed, and the compaction
/// controls.
void screen_panel_usage_layout(PanelScreen *s, Doc *doc, int w, bool after_pr) {
    const Json *raw = s->session.raw, *cu = json_get(raw, "contextUsage"), *u = usage_of(raw);
    double used = 0, window = 0, v;
    bool has_context = context_size(raw, &used, &window);
    bool active = session_is_active(&s->session), compacting = json_bool_is(json_get(raw, "compacting"), true);
    bool clear = can_op("clear") && str_eq(json_str(json_get(raw, "kind")), "devchat") && !active && !compacting;
    bool auto_shown = auto_compact_shown(raw), keep_shown = instructions_shown(raw), compact = compact_shown(raw);
    if (!has_context && !usage_rows_present(raw) && !compact && !auto_shown && !keep_shown) return;

    if (after_pr) { doc_space(doc, px(10)); doc_rule(doc, 0, w); doc_space(doc, px(10)); }
    // "Context usage", with the Auto-compact checkbox at the right of the same line.
    int y = doc->y, h = px(20);
    int label_right = w;
    if (auto_shown) {
        double at = 0; num_at(raw, "autoCompactAt", &at);
        char *label = xstrfmt("Auto-compact %dk", (int)(at / 1000 + 0.5));
        bool on = json_bool_is(json_get(raw, "autoCompact"), true);
        int lw = text_width(doc->cv, label, FONT_CAPTION) + 2, box = px(12);
        int x = w - lw - box - px(5);
        RECT br = { x, y + (h - box) / 2, x + box, y + (h - box) / 2 + box };
        int bi = doc_add(doc, &br, paint_checkbox);
        doc_item(doc, bi)->arg = on; doc_item(doc, bi)->action = ACT_AUTO_COMPACT; doc_item(doc, bi)->hand = true;
        RECT lr = { w - lw, y, w, y + h };
        int li = doc_text_at(doc, &lr, label, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        doc_item(doc, li)->action = ACT_AUTO_COMPACT; doc_item(doc, li)->hand = true;
        label_right = x - px(6);
        free(label);
    }
    RECT hr = { 0, y, label_right, y + h };
    doc_text_at(doc, &hr, "Context usage", FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc->y = y + h + px(4);
    // Instructions (accented by a ✓ while some are set), Compact and Clear.
    ButtonSpec buttons[3]; size_t nb = 0;
    if (keep_shown) {
        bool set = json_str_nonempty(json_get(raw, "compactInstructions")) != NULL;
        buttons[nb++] = (ButtonSpec){ 0, set ? "Instructions \xE2\x9C\x93" : "Instructions", BUTTON_BORDERED, ACT_INSTRUCTIONS, 0, true };
    }
    if (compact) buttons[nb++] = (ButtonSpec){ 0, compacting ? "Compacting\xE2\x80\xA6" : "Compact", BUTTON_BORDERED, ACT_COMPACT, 0, !compacting };
    // Takes the transcript off the screen only: the agent's context and the stored log stay as they are.
    if (clear) buttons[nb++] = (ButtonSpec){ 0, "Clear", BUTTON_BORDERED, ACT_CLEAR, 0, true };
    if (nb) { doc_button_row(doc, 0, w, buttons, nb); doc_space(doc, px(8)); }

    if (has_context) {
        char *u_text = format_tokens(used);
        char *headline;
        if (window > 0) { char *w_text = format_tokens(window); headline = xstrfmt("%s / %s (%d%%)", u_text, w_text, (int)(used / window * 100 + 0.5)); free(w_text); }
        else headline = xstrfmt("%s tokens", u_text);
        free(u_text);
        usage_row(doc, w, "Context", headline);
        free(headline);
        // Categories come from the claude /context probe; the other providers still get a bar, just an undivided one.
        const Json *cats = json_get(cu, "categories");
        size_t nc = 0;
        for (size_t i = 0; i < json_count(cats); i++) if (num_at(json_at(cats, i), "tokens", &v)) nc++;
        BarData *bar = xcalloc(1, sizeof *bar);
        if (nc) {
            bar->colors = xcalloc(nc, sizeof *bar->colors); bar->pct = xcalloc(nc, sizeof *bar->pct);
            for (size_t i = 0; i < json_count(cats); i++) {
                const Json *c = json_at(cats, i);
                if (!num_at(c, "tokens", &v)) continue;
                bar->colors[bar->count] = context_color(json_str(json_get(c, "name")));
                bar->pct[bar->count++] = json_num_or(json_get(c, "pct"), 0);
            }
        } else if (window > 0) {
            bar->colors = xcalloc(2, sizeof *bar->colors); bar->pct = xcalloc(2, sizeof *bar->pct);
            bar->colors[0] = context_color("context"); bar->pct[0] = used / window * 100;
            bar->colors[1] = context_color("free space"); bar->pct[1] = 100 - used / window * 100;
            bar->count = 2;
        }
        if (bar->count) { doc_space(doc, px(2)); doc_custom(doc, 0, w, px(6), paint_bar, bar, bar_free, 0, 0); doc_space(doc, px(6)); }
        else bar_free(bar);
        for (size_t i = 0; i < json_count(cats); i++) {
            const Json *c = json_at(cats, i);
            if (!num_at(c, "tokens", &v)) continue;
            const char *name = json_str(json_get(c, "name"));
            char *tokens = format_tokens(v), *pct = xstrfmt("%.1f%%", json_num_or(json_get(c, "pct"), 0));
            int ry = doc->y, rh = px(18), sq = px(8);
            int pw = px(44), tw = text_width(doc->cv, tokens, FONT_CAPTION) + 2;
            RECT sw = { 0, ry + (rh - sq) / 2, sq, ry + (rh - sq) / 2 + sq };
            int si = doc_add(doc, &sw, paint_swatch); doc_item(doc, si)->arg = (intptr_t)context_color(name);
            RECT nr = { sq + px(6), ry, w - pw - tw - px(6), ry + rh };
            doc_text_at(doc, &nr, name ? name : "", FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            RECT tr = { w - pw - tw, ry, w - pw, ry + rh };
            doc_text_at(doc, &tr, tokens, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            RECT pr = { w - pw, ry, w, ry + rh };
            doc_text_at(doc, &pr, pct, FONT_CAPTION, theme.ink, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            doc->y = ry + rh;
            free(tokens); free(pct);
        }
    }
    bool rows = usage_rows_present(raw) || num_at(u, "sessions", &v) || json_str(json_get(cu, "compactedAt"));
    if (!rows) return;
    if (has_context) { doc_space(doc, px(6)); doc_rule(doc, 0, w); doc_space(doc, px(6)); }
    char *t;
    if (num_at(u, "inputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Input tokens", t); free(t); }
    if (num_at(u, "outputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Output tokens", t); free(t); }
    if (num_at(u, "durationMs", &v)) { t = format_duration_ms(v); usage_row(doc, w, "Agent time", t); free(t); }
    if (num_at(u, "costUsd", &v)) {
        // `+`: some turns carry no price at all, so the total is a floor.
        double unpriced = 0; num_at(u, "unpricedTurns", &unpriced);
        t = xstrfmt("$%.2f%s", v, unpriced > 0 ? "+" : ""); usage_row(doc, w, "Cost", t); free(t);
    }
    // An orchestrator's figures cover its workers, a task's the reviews its loop ran.
    if (num_at(u, "sessions", &v) && v > 0) { t = xstrfmt("%d session%s it started", (int)v, (int)v == 1 ? "" : "s"); usage_row(doc, w, "Includes", t); free(t); }
    if (str_eq(json_str(json_get(cu, "source")), "codex")) {
        if (num_at(cu, "cachedInputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Thread cached input", t); free(t); }
        if (num_at(cu, "reasoningOutputTokens", &v)) { t = format_tokens(v); usage_row(doc, w, "Thread reasoning output", t); free(t); }
        if ((t = usage_time(json_get(cu, "at")))) { usage_row(doc, w, "Context updated", t); free(t); }
    }
    if ((t = usage_time(json_get(cu, "compactedAt")))) { usage_row(doc, w, "Last compact", t); free(t); }
}
