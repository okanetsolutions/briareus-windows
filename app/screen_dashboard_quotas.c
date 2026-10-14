#include "screen_dashboard_internal.h"

static bool quotas_busy(DashboardScreen *s);
static void quotas_note_retry(DashboardScreen *s, const Request *req);
static void quota_done(void *owner, Request *req);
static void accounts_done(void *owner, Request *req);
static char *quota_text(const Json *window);

// MARK: - Reading

void screen_dashboard_quotas_cancel(DashboardScreen *s) {
    request_cancel(&s->accounts_req);
    for (size_t i = 0; i < s->status_count; i++) request_cancel(&s->status_reqs[i]);
}

static bool quotas_busy(DashboardScreen *s) {
    if (s->accounts_req) return true;
    for (size_t i = 0; i < s->status_count; i++) if (s->status_reqs[i]) return true;
    return false;
}

static void quotas_note_retry(DashboardScreen *s, const Request *req) {
    if (req->ok || req->error.retry_after <= 0) return;
    double until = (double)GetTickCount64() + req->error.retry_after * 1000;
    if (until > s->quota_retry_until) s->quota_retry_until = until;
}

static void quota_done(void *owner, Request *req) {
    DashboardScreen *s = owner;
    if (req->client != g_store.client) return;
    quotas_note_retry(s, req);
    Json *row = s->accounts->a.items[req->tag];
    if (req->ok && json_is_object(json_get(req->result, "status"))) {
        json_object_set(row, "status", json_clone(json_get(req->result, "status")));
    } else {
        char *error = request_error_or_unexpected(req);
        json_set_str(row, "quotaError", error); free(error);
    }
    pane_relayout(s->base.pane);
}

static void accounts_done(void *owner, Request *req) {
    DashboardScreen *s = owner;
    if (req->client != g_store.client) return;
    quotas_note_retry(s, req);
    const Json *rows = json_get(req->result, "providers");
    if (!req->ok || !json_is_array(rows)) {
        char *error = request_error_or_unexpected(req);
        set_string(&s->accounts_error, error); free(error);
    } else {
        set_string(&s->accounts_error, NULL);
        json_free(s->accounts); s->accounts = json_clone(rows);
        free(s->status_reqs);
        s->status_count = json_count(rows);
        s->status_reqs = xcalloc(s->status_count, sizeof *s->status_reqs);
        for (size_t i = 0; i < s->status_count; i++) {
            int id = json_int_or(json_get(json_at(rows, i), "id"), 0);
            if (id <= 0) continue;
            Json *args = json_object(); json_set_num(args, "id", id);
            if (s->quota_fresh) json_set_num(args, "fresh", 1);
            store_call("provider_status", args, 60000, s, quota_done, (int)i, &s->status_reqs[i]);
        }
    }
    pane_relayout(s->base.pane);
}

void screen_dashboard_quotas_load(DashboardScreen *s, bool fresh) {
    if (!store_supports("settings_providers") || !store_supports("provider_status")) return;
    if (!fresh && (quotas_busy(s) || (double)GetTickCount64() < s->quota_retry_until)) return;
    if (fresh) screen_dashboard_quotas_cancel(s);
    s->quota_fresh = fresh;
    store_call("settings_providers", json_object(), 60000, s, accounts_done, 0, &s->accounts_req);
}

// MARK: - Tiles

static char *quota_text(const Json *window) {
    double used;
    if (!json_num(json_get(window, "usedPct"), &used) || !isfinite(used)) return xstrdup("Usage unavailable");
    double remaining = 100 - (used < 0 ? 0 : used > 100 ? 100 : used);
    time_t resets_at;
    char *reset = board_date_parse(json_str(json_get(window, "resetsAt")), &resets_at) ? format_event_time(resets_at) : NULL;
    char *text = reset ? xstrfmt("%.0f%% remaining \xC2\xB7 resets %s", remaining, reset) : xstrfmt("%.0f%% remaining", remaining);
    free(reset);
    return text;
}

void screen_dashboard_quotas_layout(DashboardScreen *s, Doc *doc, int x, int w) {
    int box = doc_box_begin(doc, x, w, px(12), theme.raise, theme.line, px(12));
    int ix = x + px(14), iw = w - px(28);
    int cols = iw >= px(1100) ? 4 : iw >= px(800) ? 3 : iw >= px(520) ? 2 : 1;
    int row_top = 0, row_bottom = 0;
    doc_text(doc, ix, iw, "Usage remaining per account", FONT_BODY, theme.ink, DT_LEFT | DT_WORDBREAK);
    doc_text(doc, ix, iw, "Current account limits across all projects, independent of the spending filters.", FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(8));
    if (!store_supports("settings_providers") || !store_supports("provider_status")) {
        doc_text(doc, ix, iw, "Account usage is unavailable on this server or with this device's permissions.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else if (s->accounts_error) {
        doc_notice(doc, ix, iw, s->accounts_error);
    } else if (!s->accounts) {
        doc_text(doc, ix, iw, "Loading account usage...", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else if (!json_count(s->accounts)) {
        doc_text(doc, ix, iw, "No provider accounts configured.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else for (size_t i = 0; i < json_count(s->accounts); i++) {
        int col = (int)(i % (size_t)cols), cw = (iw - px(16) * (cols - 1)) / cols;
        int cx = ix + col * (cw + px(16));
        if (!col) row_top = row_bottom = doc->y;
        doc->y = row_top;
        const Json *row = json_at(s->accounts, i), *status = json_get(row, "status");
        const Json *auth = json_get(status, "auth"), *usage = json_get(status, "usage"), *windows = json_get(usage, "windows");
        const char *label = json_str_nonempty(json_get(row, "label")), *binary = json_str_nonempty(json_get(row, "binary"));
        const char *email = json_str_nonempty(json_get(auth, "email")), *name = json_str_nonempty(json_get(auth, "name"));
        const char *plan = json_str_nonempty(json_get(auth, "plan"));
        char *fallback = xstrfmt("Account #%d", json_int_or(json_get(row, "id"), 0));
        char *title = xstrfmt("%s%s%s%s", label ? label : fallback, binary ? " \xC2\xB7 " : "", binary ? binary : "",
                             json_bool_is(json_get(row, "active"), false) ? " (disabled)" : "");
        doc_text(doc, cx, cw, title, FONT_BODY, theme.ink, DT_LEFT | DT_WORDBREAK);
        free(title); free(fallback);
        if (email || name || plan) {
            char *detail = xstrfmt("%s%s%s", email ? email : name ? name : "", (email || name) && plan ? " \xC2\xB7 " : "", plan ? plan : "");
            doc_text(doc, cx, cw, detail, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK); free(detail);
        }
        for (size_t j = 0; j < json_count(windows); j++) {
            const Json *window = json_at(windows, j);
            const char *label_window = json_str_nonempty(json_get(window, "label"));
            char *value = quota_text(window), *line = xstrfmt("%s: %s", label_window ? label_window : "Quota", value);
            doc_text(doc, cx, cw, line, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
            free(value); free(line);
        }
        if (!json_count(windows)) {
            const char *error = json_str_nonempty(json_get(row, "quotaError"));
            if (!error) error = json_str_nonempty(json_get(usage, "error"));
            doc_text(doc, cx, cw, error ? error : s->status_reqs[i] ? "Checking usage..." : "Usage unavailable for this account.",
                     FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        }
        if (doc->y > row_bottom) row_bottom = doc->y;
        doc->y = row_bottom;
        if (col == cols - 1 && i + 1 < json_count(s->accounts)) doc_space(doc, px(12));
    }
    doc_box_end(doc, box, px(12));
    doc_space(doc, px(12));
}
