#include "screen_dashboard_internal.h"

static void usage_done(void *owner, Request *req);
static void load(DashboardScreen *s);
static void restore(DashboardScreen *s);
static void dashboard_layout(Screen *base, Doc *doc);
static void dashboard_header(Screen *base, HeaderInfo *info);
static void open_top_session(DashboardScreen *s, size_t index);
static void dashboard_action(Screen *base, int action, intptr_t arg, POINT pt);
static void dashboard_timer(Screen *base, UINT id);
static void dashboard_visible(Screen *base, bool shown_now);
static void dashboard_refresh(Screen *base);
static void dashboard_activated(Screen *base, bool active);
static void dashboard_destroy(Screen *base);

// The window is remembered while the app runs; each screen owns its active query.
UsageWindow screen_dashboard_preference;

static void usage_done(void *owner, Request *req) {
    DashboardScreen *s = owner;
    if (req->ok && json_is_object(req->result)) {
        json_free(s->data); free(s->data_key);
        s->data = req->result; req->result = NULL;
        s->data_key = json_serialize(req->args, true);
        if (json_is_object(json_get(s->data, "options"))) { json_free(s->options); s->options = json_clone(json_get(s->data, "options")); }
        const char *key = usage_query_cache_key(req->args);
        if (key) cache_store(g_store.cache, s->data, key);
        set_string(&s->error, NULL);
    } else {
        char *text = request_error_or_unexpected(req);
        set_string(&s->error, text); free(text);
    }
    poller_finished(&s->poller, !req->ok, req->error.retry_after);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
}

static void load(DashboardScreen *s) {
    request_cancel(&s->req);
    if (!store_supports("usage_all")) return;
    store_call("usage_all", screen_dashboard_query_args(s), 60000, s, usage_done, 0, &s->req);
}

/// A new window or pick: straight to the loader, so the page never reads as the old pick's, and ask again.
void screen_dashboard_repick(DashboardScreen *s) {
    set_string(&s->error, NULL);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    load(s);
}

static void restore(DashboardScreen *s) {
    Json *args = usage_window_args(&s->window);
    const char *key = usage_query_cache_key(args);
    json_free(args);
    if (!key) return;
    Json *saved = cache_value(g_store.cache, key);
    if (!json_is_object(saved)) { json_free(saved); return; }
    s->data = saved; s->data_key = screen_dashboard_query_key(s);
    if (json_is_object(json_get(saved, "options"))) s->options = json_clone(json_get(saved, "options"));
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
        texts[f] = screen_dashboard_filter_button_text(s, (FilterId)f);
        ButtonSpec b = { 0, texts[f], BUTTON_BORDERED, ACT_FILTER, f, s->options != NULL };
        buttons[f] = b;
    }
    ButtonSpec clear = { 0, "Clear filters", BUTTON_BORDERED, ACT_CLEAR, 0, screen_dashboard_any_pick(s) };
    buttons[F_COUNT] = clear;
    doc_button_row(doc, x, w, buttons, F_COUNT + 1);
    for (int f = 0; f < F_COUNT; f++) free(texts[f]);
    doc_space(doc, px(14));

    screen_dashboard_quotas_layout(s, doc, x, w);
    const Json *u = screen_dashboard_shown(s);
    if (s->error && !u) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); return; }
    if (!u) { doc_text(doc, x, w, "Loading the usage ledger\xE2\x80\xA6", FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_SINGLELINE); doc_space(doc, px(12)); return; }
    if (s->error) { doc_notice(doc, x, w, s->error); doc_space(doc, px(10)); }
    if (!screen_dashboard_count_of(u, "turns")) {
        // An empty window still lists the projects, at zero: "which projects ran nothing?" is the question left.
        char *over = screen_dashboard_window_name(u, &s->window);
        char *line = xstrfmt("No usage recorded in %s%s.", over, screen_dashboard_any_pick(s) ? " for these filters" : "");
        doc_space(doc, px(18));
        doc_text(doc, x, w, line, FONT_FOOTNOTE, theme.muted, DT_CENTER | DT_WORDBREAK);
        doc_space(doc, px(18));
        free(line); free(over);
        screen_dashboard_insights(doc, x, w, u);
        screen_dashboard_project_card(s, doc, x, w, u);
        doc_space(doc, px(16));
        return;
    }
    static const char *const labels[] = { "Cost", "Tokens", "Sessions", "Agent time" };
    char *values[4], *subs[4];
    values[0] = screen_dashboard_cost_or_dash(u); subs[0] = screen_dashboard_cost_note(u);
    values[1] = screen_dashboard_tokens_text(screen_dashboard_num(u, "totalTokens"));
    { char *in = screen_dashboard_tokens_text(screen_dashboard_num(u, "inputTokens")), *out = screen_dashboard_tokens_text(screen_dashboard_num(u, "outputTokens")); subs[1] = xstrfmt("%s in \xC2\xB7 %s out", in, out); free(in); free(out); }
    values[2] = xstrfmt("%d", screen_dashboard_count_of(u, "sessions"));
    subs[2] = xstrfmt("%d turn%s", screen_dashboard_count_of(u, "turns"), screen_dashboard_plural(screen_dashboard_count_of(u, "turns")));
    values[3] = screen_dashboard_duration_or_dash(screen_dashboard_num(u, "durationMs")); subs[3] = xstrdup("summed over every turn");
    screen_dashboard_tiles(doc, x, w, labels, values, subs, 4);
    for (int k = 0; k < 4; k++) { free(values[k]); free(subs[k]); }
    bool month = str_eq(json_str(json_get(u, "unit")), "month");
    screen_dashboard_chart(doc, x, w, u, 0, "totalTokens", month ? "Tokens per month" : "Tokens per day");
    screen_dashboard_insights(doc, x, w, u);
    screen_dashboard_project_card(s, doc, x, w, u);
    screen_dashboard_simple_card(s, doc, x, w, u, F_ACTIVITY);
    screen_dashboard_simple_card(s, doc, x, w, u, F_PROVIDER);
    screen_dashboard_simple_card(s, doc, x, w, u, F_MODEL);
    doc_space(doc, px(16));
}

static void dashboard_header(Screen *base, HeaderInfo *info) {
    DashboardScreen *s = (DashboardScreen *)base;
    snprintf(info->title, sizeof info->title, "\xF0\x9F\x93\x8A Dashboard");
    const Json *u = screen_dashboard_shown(s);
    if (u) {
        // A filter is said in the subtitle as well as in its picker: these totals are a slice, and this line says which.
        Str sub; str_init(&sub);
        char *over = screen_dashboard_window_name(u, &s->window);
        str_appendz(&sub, over); free(over);
        bool picked = false;
        for (int f = 0; f < F_COUNT; f++) {
            if (!s->pick_count[f]) continue;
            char *l = screen_dashboard_picks_label(s, (FilterId)f);
            str_appendf(&sub, " \xC2\xB7 only %s", l); free(l);
            picked = true;
        }
        if (!picked) {
            int active = 0;
            const Json *projects = json_get(u, "projects");
            for (size_t i = 0; i < json_count(projects); i++) if (screen_dashboard_count_of(json_at(projects, i), "turns")) active++;
            str_appendf(&sub, " \xC2\xB7 %d project%s with usage", active, screen_dashboard_plural(active));
        }
        str_appendf(&sub, " \xC2\xB7 %d turn%s", screen_dashboard_count_of(u, "turns"), screen_dashboard_plural(screen_dashboard_count_of(u, "turns")));
        snprintf(info->subtitle, sizeof info->subtitle, "%s", sub.data);
        str_free(&sub);
    }
    if (!store_supports("usage_all")) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    char *label = usage_window_label(&s->window);
    snprintf(b->label, sizeof b->label, "%s \xE2\x96\xBE", label); free(label);
    b->glyph = 0xE787; b->action = ACT_PERIOD; b->enabled = true; b->tip = "The window every number on this page is over";
    HeaderButton *r = &info->buttons[info->button_count++];
    r->glyph = 0xE72C; r->action = ACT_REFRESH; r->enabled = !s->req; r->tip = "Read the usage ledger again";
}

static void open_top_session(DashboardScreen *s, size_t index) {
    const Json *u = screen_dashboard_shown(s);
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
    if (session_parse(raw, &ses)) { app_push_from(&s->base, conversation_screen_new(&ses)); session_free(&ses); }
    json_free(raw);
}

static void dashboard_action(Screen *base, int action, intptr_t arg, POINT pt) {
    DashboardScreen *s = (DashboardScreen *)base;
    switch (action) {
    case ACT_PERIOD: screen_dashboard_pick_period(s, pt); break;
    case ACT_REFRESH: load(s); screen_dashboard_quotas_load(s, true); pane_header_changed(base->pane); break;
    case ACT_FILTER: if (arg >= 0 && arg < F_COUNT) screen_dashboard_pick_filter(s, (FilterId)arg, pt); break;
    case ACT_CLEAR: for (int f = 0; f < F_COUNT; f++) screen_dashboard_picks_clear(s, (FilterId)f); screen_dashboard_repick(s); break;
    case ACT_OPEN_SESSION: open_top_session(s, (size_t)arg); break;
    case ACT_ROW: {
        const Json *u = screen_dashboard_shown(s);
        FilterId f = (FilterId)(arg / ROW_STRIDE);
        size_t i = (size_t)(arg % ROW_STRIDE);
        if (!u || f < 0 || f >= F_COUNT) break;
        const char *list = f == F_PROJECT ? "projects" : f == F_ACTIVITY ? "activities" : f == F_PROVIDER ? "providers" : "models";
        const Json *r = json_at(json_get(u, list), i);
        if (json_is_null(r)) break;
        const char *key = f == F_ACTIVITY ? json_str_nonempty(json_get(r, "activity")) : f == F_PROVIDER ? json_str_nonempty(json_get(r, "provider")) : screen_dashboard_option_key(r);
        screen_dashboard_pick_only(s, f, key ? key : "unknown");
        screen_dashboard_repick(s);
        break;
    }
    }
}

static void dashboard_timer(Screen *base, UINT id) {
    DashboardScreen *s = (DashboardScreen *)base;
    if (!poller_fired(&s->poller, id)) return;
    screen_dashboard_quotas_load(s, false);
    if (s->req) poller_finished(&s->poller, false, -1); else load(s);
}

static void dashboard_visible(Screen *base, bool shown_now) {
    DashboardScreen *s = (DashboardScreen *)base;
    if (shown_now) poller_start(&s->poller, base->pane, TIMER_POLL, 60000);
    else { poller_stop(&s->poller); request_cancel(&s->req); screen_dashboard_quotas_cancel(s); }
}

static void dashboard_refresh(Screen *base) { DashboardScreen *s = (DashboardScreen *)base; load(s); screen_dashboard_quotas_load(s, true); pane_header_changed(base->pane); }

static void dashboard_activated(Screen *base, bool active) { if (active) { DashboardScreen *s = (DashboardScreen *)base; poller_start(&s->poller, base->pane, TIMER_POLL, 60000); } }

static void dashboard_destroy(Screen *base) {
    DashboardScreen *s = (DashboardScreen *)base;
    poller_stop(&s->poller);
    request_cancel(&s->req);
    screen_dashboard_quotas_cancel(s);
    json_free(s->accounts); free(s->accounts_error); free(s->status_reqs);
    json_free(s->data); json_free(s->options); free(s->data_key); free(s->error);
    for (int f = 0; f < F_COUNT; f++) screen_dashboard_picks_clear(s, (FilterId)f);
    screen_release(base);
}

static const ScreenVTable dashboard_vt = {
    .destroy = dashboard_destroy, .layout = dashboard_layout, .header = dashboard_header, .action = dashboard_action,
    .timer = dashboard_timer, .visible = dashboard_visible, .refresh = dashboard_refresh, .activated = dashboard_activated,
    .detachable = true,
};

Screen *dashboard_screen_new(void) {
    DashboardScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &dashboard_vt; s->base.id = xstrdup("dashboard");
    s->window = screen_dashboard_preference;
    restore(s);
    return &s->base;
}
