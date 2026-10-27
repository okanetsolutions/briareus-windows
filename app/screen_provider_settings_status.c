#include "screen_provider_settings_internal.h"

typedef struct { char *text; const char *dot; } HeadlineData;

typedef struct { char *label, *right; double pct; } BarData;

static char *format_when(const char *iso);
static char *status_headline(const Json *status, const char **dot);
static void headline_free(void *p);
static void paint_headline(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void status_row(Doc *doc, int x, int w, const char *label, const char *value, bool mono);
static void bar_free(void *p);
static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void status_done(void *owner, Request *req);
static void login_done(void *owner, Request *req);
static void finish_done(void *owner, Request *req);
static void test_done(void *owner, Request *req);

/// When a time falls: the clock today, the weekday and the clock on another day, as the dashboard prints resets.
static char *format_when(const char *iso) {
    time_t t;
    if (!iso || !board_date_parse(iso, &t)) return NULL;
    struct tm when = *localtime(&t), now; time_t n = time(NULL); now = *localtime(&n);
    bool today = when.tm_year == now.tm_year && when.tm_yday == now.tm_yday;
    char buf[64];
    strftime(buf, sizeof buf, today ? "%H:%M" : "%a %H:%M", &when);
    return xstrdup(buf);
}

/// The connection at a glance, as the dashboard's status headline: a dot and a sentence. NULL before a status was read.
static char *status_headline(const Json *status, const char **dot) {
    if (!json_is_object(status)) return NULL;
    const Json *auth = json_get(status, "auth");
    const Json *logged = json_get(auth, "loggedIn");
    bool available = !json_bool_is(json_get(status, "available"), false);
    const char *email = json_str_nonempty(json_get(auth, "email")), *detail = json_str_nonempty(json_get(auth, "detail"));
    *dot = !available || json_bool_is(logged, false) ? "failed" : json_bool_is(logged, true) ? "idle" : "";
    if (!available) return xstrdup("The CLI is not installed on this machine");
    if (json_bool_is(logged, true)) return email ? xstrfmt("Connected: %s", email) : xstrdup("Connected");
    if (json_bool_is(logged, false)) return detail ? xstrfmt("Not connected: %s", detail) : xstrdup("Not connected");
    return xstrdup("Connection not checked yet");
}

static void headline_free(void *p) { HeadlineData *d = p; free(d->text); free(d); }

static void paint_headline(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    HeadlineData *d = it->data;
    int cy = (rc->top + rc->bottom) / 2;
    draw_status_dot(cv, rc->left + px(4), cy, d->dot);
    RECT t = { rc->left + px(16), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void screen_provider_settings_headline(FormScreen *s, Doc *doc, int x, int w) {
    const char *dot = "";
    char *text = status_headline(s->status, &dot);
    if (!text) { doc_text(doc, x, w, s->status_error ? s->status_error : "Checking the connection\xE2\x80\xA6", FONT_FOOTNOTE, s->status_error ? theme.danger : theme.muted, DT_LEFT | DT_WORDBREAK); return; }
    HeadlineData *d = xcalloc(1, sizeof *d); d->text = text; d->dot = dot;
    doc_custom(doc, x, w, px(24), paint_headline, d, headline_free, 0, 0);
}

/// A label and its value, in the dashboard's two columns; nothing when there is no value.
static void status_row(Doc *doc, int x, int w, const char *label, const char *value, bool mono) {
    if (str_empty(value)) return;
    int lw = px(150), top = doc->y;
    RECT l = { x, top, x + lw, top + font_height(doc->cv, FONT_FOOTNOTE) };
    doc_text_at(doc, &l, label, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE);
    doc_text(doc, x + lw + px(14), w - lw - px(14), value, mono ? FONT_MONO : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
    if (doc->y < l.bottom) doc->y = l.bottom;
    doc_space(doc, px(3));
}

static void bar_free(void *p) { BarData *d = p; free(d->label); free(d->right); free(d); }

static void paint_bar(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    BarData *d = it->data;
    int th = font_height(cv, FONT_CAPTION);
    RECT l = { rc->left, rc->top, rc->right, rc->top + th };
    draw_text(cv, d->label, &l, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(cv, d->right, &l, FONT_CAPTION, theme.muted, DT_RIGHT | DT_SINGLELINE);
    RECT track = { rc->left, l.bottom + px(4), rc->right, l.bottom + px(9) };
    fill_round_rect(cv, &track, px(3), theme.sunken, theme.sunken);
    double pct = d->pct < 0 ? 0 : d->pct > 100 ? 100 : d->pct;
    RECT fill = track; fill.right = track.left + (int)((track.right - track.left) * pct / 100.0);
    COLORREF c = pct >= 90 ? theme.danger : pct >= 70 ? theme.warn : theme.ok;
    if (fill.right > fill.left) fill_round_rect(cv, &fill, px(3), c, c);
}

/// The Status tab: who the provider is logged in as, its plan, where its CLI lives, and the quota windows its plan meters.
void screen_provider_settings_layout_status(FormScreen *s, Doc *doc, int x, int w) {
    screen_provider_settings_headline(s, doc, x, w);
    doc_space(doc, px(12));
    if (json_is_object(s->status)) {
        const Json *auth = json_get(s->status, "auth");
        const char *email = json_str_nonempty(json_get(auth, "email")), *name = json_str_nonempty(json_get(auth, "name"));
        char *account = email && name ? xstrfmt("%s (%s)", email, name) : xstrdup(email ? email : name ? name : "");
        char *plan = json_str_nonempty(json_get(auth, "plan")) ? str_capitalized(json_str(json_get(auth, "plan"))) : xstrdup("");
        bool available = !json_bool_is(json_get(s->status, "available"), false);
        char *checked = format_when(json_str(json_get(auth, "checkedAt")));
        status_row(doc, x, w, "Account", account, false);
        status_row(doc, x, w, "Organization", json_str(json_get(auth, "organization")), false);
        status_row(doc, x, w, "Plan", plan, false);
        const Json *logged = json_get(auth, "loggedIn");
        if (logged && !json_is_null(logged)) status_row(doc, x, w, "Auth", json_str(json_get(auth, "detail")), false);
        status_row(doc, x, w, "Binary", available ? json_str(json_get(s->status, "binSource")) : "not found", false);
        status_row(doc, x, w, "Login dir", json_str(json_get(s->status, "loginDir")), true);
        status_row(doc, x, w, "Checked", checked, false);
        free(account); free(plan); free(checked);
        // Which windows a plan meters is the provider's call, so the bars wear the labels that came with them.
        const Json *windows = json_get(json_get(s->status, "usage"), "windows");
        for (size_t i = 0; i < json_count(windows); i++) {
            const Json *win = json_at(windows, i);
            double pct = 0; json_num(json_get(win, "usedPct"), &pct);
            char *reset = format_when(json_str(json_get(win, "resetsAt")));
            BarData *d = xcalloc(1, sizeof *d);
            const char *label = json_str(json_get(win, "label"));
            d->label = xstrdup(label ? label : "");
            d->right = reset ? xstrfmt("%.0f%% used \xC2\xB7 resets %s", pct, reset) : xstrfmt("%.0f%% used", pct);
            d->pct = pct;
            free(reset);
            doc_space(doc, px(8));
            doc_custom(doc, x, w, font_height(doc->cv, FONT_CAPTION) + px(9), paint_bar, d, bar_free, 0, 0);
        }
        if (!json_count(windows)) {
            const char *why = json_str_nonempty(json_get(json_get(s->status, "usage"), "error"));
            doc_space(doc, px(8));
            doc_text(doc, x, w, why ? why : "Usage unavailable: this account\xE2\x80\x99s meter could not be read or is not supported.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        }
        doc_space(doc, px(16));
    }
    bool busy = s->req_status != NULL;
    doc_button(doc, x, 0, busy && s->status_fresh ? "Checking usage\xE2\x80\xA6" : "Check usage", BUTTON_BORDERED, ACT_CHECK, 0, !busy && store_supports("provider_status"));
    doc_space(doc, px(8));
    screen_provider_settings_note(doc, x, w, "Check usage reads the account and its quota again rather than the server's last answer.");
}

// MARK: Status, login and Test

static void status_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) { request_error_into(&s->status_error, req); }
    else {
        json_free(s->status); s->status = json_clone(json_get(req->result, "status"));
        set_string(&s->status_error, NULL);
        // A login that landed shows in the sidebar's own login tag.
        if (s->device_code && provider_login_landed(s->login_from, s->status)) { set_string(&s->device_code, NULL); settings_providers_changed(false); }
    }
    pane_relayout(s->base.pane);
}

void screen_provider_settings_load_status(FormScreen *s, bool fresh) {
    if (!s->id || s->req_login || !store_supports("provider_status")) return;
    request_cancel(&s->req_status);
    Json *args = json_object(); json_set_num(args, "id", s->id);
    if (fresh) json_set_num(args, "fresh", 1);
    s->status_fresh = fresh;
    store_call("provider_status", args, 0, s, status_done, 0, &s->req_status);
    pane_relayout(s->base.pane);
}

static void login_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) { request_error_into(&s->error, req); pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    set_string(&s->error, NULL);
    const char *url = json_str_nonempty(json_get(req->result, "url"));
    if (str_eq(s->binary, "claude")) {
        // claude.ai shows a code once approved; it comes back here to finish.
        if (url) { open_web_url(url); s->code_wanted = true; s->focus_first = F_LOGIN_CODE; }
    } else if (!url) {
        app_alert("Already logged in", "This entry is already logged in.");
        screen_provider_settings_load_status(s, false);
    } else {
        // Device auth prints the URL and waits, so the app opens it; the status is read again a few times to catch the login.
        open_web_url(url);
        const char *code = json_str_nonempty(json_get(req->result, "deviceCode"));
        set_string(&s->device_code, code);
        s->login_wait = 0;
        SetTimer(pane_hwnd(s->base.pane), TIMER_LOGIN, (UINT)screen_provider_settings_LOGIN_WAITS_MS[0], NULL);
    }
    pane_relayout(s->base.pane);
    if (s->focus_first >= 0 && s->edits[s->focus_first]) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
}

void screen_provider_settings_cancel_login(FormScreen *s) {
    // Picker changes must preserve the initial read, but discard polls from a login attempt.
    if (s->req_login || s->login_from || s->device_code || s->code_wanted) request_cancel(&s->req_status);
    request_cancel(&s->req_login);
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_LOGIN);
    json_free(s->login_from); s->login_from = NULL;
    set_string(&s->device_code, NULL); s->code_wanted = false;
}

void screen_provider_settings_login_start(FormScreen *s) {
    if (!screen_provider_settings_can_log_in(s) || s->req_login) return;
    // Wait for the initial read so a late old login cannot be mistaken for this attempt.
    if (str_eq(s->binary, "codex") && (!json_is_object(s->status) || s->req_status)) return;
    const char *op = str_eq(s->binary, "claude") ? "provider_login_start" : "provider_login";
    if (!store_supports(op)) return;
    request_cancel(&s->req_status);
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_LOGIN);
    json_free(s->login_from); s->login_from = json_clone(json_get(s->status, "auth"));
    set_string(&s->device_code, NULL); s->code_wanted = false;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call(op, args, 0, s, login_done, 0, &s->req_login);
    pane_relayout(s->base.pane);
}

static void finish_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "provider") : NULL;
    if (!json_is_object(row)) {
        char *t = request_error_or_unexpected(req);
        set_string(&s->error, t); free(t);
        pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    set_string(&s->error, NULL);
    s->code_wanted = false;
    if (s->edits[F_LOGIN_CODE]) screen_provider_settings_set_edit_text(s->edits[F_LOGIN_CODE], "");
    // The saved row now carries its login; the form keeps any change not saved yet.
    json_set_bool(s->row, "hasLogin", true);
    const char *dir = json_str(json_get(row, "loginDir"));
    if (dir) json_set_str(s->row, "loginDir", dir);
    screen_provider_settings_load_status(s, false);
    settings_providers_changed(false);
    const char *label = json_str_nonempty(json_get(row, "label"));
    char *message = xstrfmt("%s is logged in.", label ? label : "The provider");
    app_alert("Logged in", message);
    free(message);
}

void screen_provider_settings_login_finish(FormScreen *s) {
    if (!s->code_wanted || s->req_login || !s->edits[F_LOGIN_CODE] || !store_supports("provider_login_finish")) return;
    char *text = screen_provider_settings_edit_text(s->edits[F_LOGIN_CODE]), *code = str_trim(text); free(text);
    if (!*code) { set_string(&s->error, "Paste the code from the browser first."); free(code); pane_relayout(s->base.pane); return; }
    Json *args = json_object(); json_set_num(args, "id", s->id); json_set_str(args, "code", code);
    free(code);
    store_call("provider_login_finish", args, 0, s, finish_done, 0, &s->req_login);
    pane_relayout(s->base.pane);
}

static void test_done(void *owner, Request *req) {
    FormScreen *s = owner;
    s->test_failed = !req->ok;
    if (!req->ok) { request_error_into(&s->test_result, req); pane_relayout(s->base.pane); return; }
    const char *probed = json_str_nonempty(json_get(req->result, "probedModel"));
    if (probed) {
        char *t = xstrfmt("OK: no model list route, but a chat call as %s answered", probed);
        set_string(&s->test_result, t); free(t);
        pane_relayout(s->base.pane);
        return;
    }
    // The endpoint's own model list goes into the Models box, unsaved, so a bad list is one discard away.
    const Json *models = json_get(req->result, "models");
    size_t n = json_count(models);
    Str list; str_init(&list);
    for (size_t i = 0; i < n; i++) { const char *m = json_str(json_at(models, i)); if (!m) continue; if (list.len) str_appendc(&list, '\n'); str_appendz(&list, m); }
    char *joined = list.data ? str_detach(&list) : xstrdup("");
    char *now = s->edits[F_MODELS] ? screen_provider_settings_field_now(s, F_MODELS) : xstrdup("");
    bool same = str_eq(now, joined);
    if (!same && s->edits[F_MODELS]) { screen_provider_settings_set_edit_text(s->edits[F_MODELS], joined); s->lines[F_MODELS] = screen_provider_settings_line_count(joined); }
    char *t = xstrfmt("OK: the endpoint offers %zu model%s%s", n, n == 1 ? "" : "s", same ? "; the Models tab already matches" : "; they are in the Models tab, save to keep them");
    set_string(&s->test_result, t);
    free(t); free(now); free(joined);
    pane_relayout(s->base.pane);
}

void screen_provider_settings_test_endpoint(FormScreen *s) {
    if (s->req_test || !store_supports("test_provider")) return;
    Json *args = json_object();
    json_set_str(args, "binary", s->binary);
    char *v = screen_provider_settings_field_now(s, F_BASE_URL); json_set_str(args, "baseUrl", v); free(v);
    v = s->edits[F_API_KEY] ? screen_provider_settings_edit_text(s->edits[F_API_KEY]) : xstrdup(""); json_set_str(args, "apiKey", v); free(v);
    v = screen_provider_settings_field_now(s, F_DEFAULT_MODEL); json_set_str(args, "defaultModel", v); free(v);
    v = screen_provider_settings_field_now(s, F_MODELS); json_object_set(args, "models", screen_provider_settings_list_from_text(v)); free(v);
    // The saved row's id lets the server probe as the model it resolves for it.
    if (s->id) json_set_num(args, "id", s->id);
    set_string(&s->test_result, "Testing\xE2\x80\xA6"); s->test_failed = false;
    store_call("test_provider", args, 0, s, test_done, 0, &s->req_test);
    pane_relayout(s->base.pane);
}
