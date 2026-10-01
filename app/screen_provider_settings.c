// One provider's settings, as the dashboard's provider form laid out as the project form is: tabs along the top
// (Provider, Connection, Models and, once saved, Status), saved through /settings/providers. A provider is one login or
// endpoint of one CLI; its login happens in the browser and its connection and quota are read from the server.
#include "screens.h"
#include "str.h"
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }
static char *form_id(int id) { return id > 0 ? xstrfmt("settings-provider:%d", id) : xstrdup("settings-provider:new"); }

/// The CLIs a provider runs, as the dashboard's Binary select offers them.
static const struct { const char *id, *title; } BINARIES[] = {
    { "claude", "claude (Claude Code)" }, { "codex", "codex (Codex)" }, { "grok", "grok (Grok)" }, { "opencode", "opencode" },
};
enum { BINARY_COUNT = sizeof BINARIES / sizeof *BINARIES };

typedef enum { K_TEXT, K_LIST } FieldKind;
/// One of the form's boxes: the Provider key it edits (NULL for the login code, which is not part of the row).
typedef struct { const char *key; FieldKind kind; const char *label, *cue; int rows; bool mono, secret; } FieldDef;
enum { F_LABEL, F_BASE_URL, F_API_KEY, F_MODELS, F_EFFORTS, F_DEFAULT_MODEL, F_DEFAULT_EFFORT, F_LOGIN_CODE, F_COUNT };
static const FieldDef FIELDS[F_COUNT] = {
    [F_LABEL] = { "label", K_TEXT, "Label", "shown in the provider picker", 0, false, false },
    [F_BASE_URL] = { "baseUrl", K_TEXT, "Base URL", "empty = the binary's own service", 0, true, false },
    [F_API_KEY] = { "apiKey", K_TEXT, "API token", NULL, 0, true, true },
    [F_MODELS] = { "models", K_LIST, "Models", NULL, 4, true, false },
    [F_EFFORTS] = { "efforts", K_LIST, "Efforts", NULL, 4, true, false },
    [F_DEFAULT_MODEL] = { "defaultModel", K_TEXT, "Default model", "empty = the CLI's default", 0, true, false },
    [F_DEFAULT_EFFORT] = { "defaultEffort", K_TEXT, "Default effort", "empty = the CLI's default", 0, true, false },
    [F_LOGIN_CODE] = { NULL, K_TEXT, "Code from the browser", "paste the code from the browser", 0, true, false },
};

/// The dashboard's sections as tabs, as the project form has them. Status is a saved provider's alone.
enum { T_PROVIDER, T_CONNECTION, T_MODELS, T_STATUS, T_COUNT };
static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_PROVIDER] = { "Provider", 0xE713 }, [T_CONNECTION] = { "Connection", 0xE71B },
    [T_MODELS] = { "Models", 0xE8FD }, [T_STATUS] = { "Status", 0xE9D9 },
};
/// The open tab stays open from one provider to the next.
static int g_tab;
static int field_tab(int f) {
    switch (f) {
    case F_LABEL: return T_PROVIDER;
    case F_BASE_URL: case F_API_KEY: case F_LOGIN_CODE: return T_CONNECTION;
    default: return T_MODELS;
    }
}

enum { ACT_SAVE = 1200, ACT_CLONE, ACT_DELETE, ACT_TAB, ACT_ACTIVE, ACT_BINARY, ACT_MODE, ACT_FOCUS, ACT_LOGIN, ACT_FINISH, ACT_TEST, ACT_CHECK, ACT_COPY_CODE };
enum { ID_FIELD = 2100 };
enum { TIMER_LOGIN = 7 };
/// After a device login is started, the status is read again this long after it, then after each next wait.
static const int LOGIN_WAITS_MS[] = { 10000, 30000, 60000, 120000 };

typedef struct {
    Screen base;
    Json *row;             // what the form was filled from: the saved row, the defaults, or a clone's values
    int id;                // 0 until the provider is saved
    HWND edits[F_COUNT];
    RECT rects[F_COUNT];
    bool laid[F_COUNT], clipped[F_COUNT];
    int lines[F_COUNT];
    bool active, token;    // Active, and the Mode: an API token rather than the CLI's own login
    char *binary;
    Json *status;          // GET /settings/providers/{id}/status's `status`
    char *status_error;
    bool status_fresh;     // the read under way was asked with `fresh`
    char *test_result; bool test_failed;
    char *device_code;     // codex's code to type on its confirm page
    bool code_wanted;      // a claude login waits for the code its authorization page shows
    int login_wait;        // the next of LOGIN_WAITS_MS
    Request *req_save, *req_delete, *req_status, *req_login, *req_test;
    bool dirty, filling, shown, tab_dot;
    int focused, focus_first;
    char *error;
} FormScreen;

enum { MAX_ROWS = 40 };
static int line_count(const char *text) { int n = 1; for (const char *c = text; c && *c; c++) if (*c == '\n') n++; return n; }
static bool is_multiline(int f) { return FIELDS[f].kind == K_LIST; }
static int shown_rows(FormScreen *s, int f) { int n = s->lines[f] > FIELDS[f].rows ? s->lines[f] : FIELDS[f].rows; return n < MAX_ROWS ? n : MAX_ROWS; }

// MARK: What the binary and the mode allow

/// grok has no token mode and opencode nothing but a key, so only claude and codex choose.
static bool mode_offered(const char *binary) { return !str_eq(binary, "grok") && !str_eq(binary, "opencode"); }
/// Whether the endpoint and its token are part of the provider: the API token mode, or opencode's key.
static bool uses_token(FormScreen *s) { return str_eq(s->binary, "opencode") || (!str_eq(s->binary, "grok") && s->token); }
/// A login is registered against a saved row, so only a saved one logged in through its CLI can log in.
static bool can_log_in(FormScreen *s) { return s->id && !uses_token(s) && !str_eq(s->binary, "opencode"); }
static bool field_shown(FormScreen *s, int f) {
    if (f == F_BASE_URL || f == F_API_KEY) return uses_token(s);
    if (f == F_LOGIN_CODE) return s->code_wanted && can_log_in(s);
    return true;
}
static const char *binary_title(const char *binary) {
    for (size_t i = 0; i < BINARY_COUNT; i++) if (str_eq(BINARIES[i].id, binary)) return BINARIES[i].title;
    return binary && *binary ? binary : "Pick a binary";
}
static const char *base_url_hint(const char *binary) {
    if (str_eq(binary, "claude")) return "Passed to the claude CLI as `ANTHROPIC_BASE_URL`, for a proxy or an Anthropic-compatible endpoint. Leave empty for the Anthropic API.";
    if (str_eq(binary, "opencode")) return "The base URL of the service this entry's models name (`anthropic` for `anthropic/claude-sonnet-4-5`), for a proxy or a compatible gateway; include the `/v1` when the service's own URL carries one. Handed to the CLI as an inline config (`OPENCODE_CONFIG_CONTENT`) alongside the key, so the machine's own opencode config is never touched. Leave empty for the service's own endpoint.";
    return "A custom API endpoint driven through the codex CLI (Responses wire format). Sessions run with a server-written `CODEX_HOME` pointing codex at it, so the machine's own `~/.codex` login is never touched. Leave empty for the binary's own service.";
}
static const char *api_key_hint(const char *binary) {
    if (str_eq(binary, "claude")) return "Passed as `ANTHROPIC_API_KEY`, used instead of a login. With a base URL it also goes out as `ANTHROPIC_AUTH_TOKEN`, so gateways that read `Authorization` instead of `x-api-key` see it too.";
    if (str_eq(binary, "opencode")) return "The key for the service this entry's models name: `anthropic` for `anthropic/claude-sonnet-4-5`. It is handed to the CLI as its whole credential store, so the machine's own opencode credentials stay untouched.";
    return "Authenticates the base URL above.";
}

// MARK: Reading and writing the fields

static char *edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}
static void set_edit_text(HWND edit, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(edit, w);
    free(w); free(crlf);
}
/// A field's value in the row, as the edit shows it: a list one item per line.
static char *field_text(const Json *row, int f) {
    if (!FIELDS[f].key) return xstrdup("");
    const Json *v = json_get(row, FIELDS[f].key);
    if (FIELDS[f].kind == K_LIST) {
        Str out; str_init(&out);
        for (size_t i = 0; i < json_count(v); i++) { const char *line = json_str(json_at(v, i)); if (!line) continue; if (out.len) str_appendc(&out, '\n'); str_appendz(&out, line); }
        return out.data ? str_detach(&out) : xstrdup("");
    }
    return xstrdup(json_str(v) ? json_str(v) : "");
}
static Json *list_from_text(const char *text) {
    Json *list = json_array();
    size_t n; char **lines = str_split(text, '\n', &n);
    for (size_t i = 0; i < n; i++) { char *t = str_trim(lines[i]); if (*t) json_array_push(list, json_string(t)); free(t); }
    str_array_free(lines, n);
    return list;
}
static const char *row_binary(const Json *row) { const char *b = json_str_nonempty(json_get(row, "binary")); return b ? b : "claude"; }
/// Fills every field from `s->row`: the edits once they exist, the flags and the mode at once. The mode is not stored: a
/// row with a token or an endpoint is in API token mode, every other one logs in.
static void form_fill(FormScreen *s) {
    s->active = !json_bool_is(json_get(s->row, "active"), false);
    set_string(&s->binary, row_binary(s->row));
    s->token = json_str_nonempty(json_get(s->row, "apiKey")) || json_str_nonempty(json_get(s->row, "baseUrl"));
    s->filling = true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !FIELDS[f].key) continue;
        char *text = field_text(s->row, f);
        set_edit_text(s->edits[f], text);
        s->lines[f] = line_count(text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
}
/// A field's value as the form would send it, as a new string: trimmed, and the endpoint's emptied when the mode drops it.
static char *field_now(FormScreen *s, int f) {
    if (!s->edits[f]) return field_text(s->row, f);
    if ((f == F_BASE_URL || f == F_API_KEY) && !uses_token(s)) return xstrdup("");
    char *text = edit_text(s->edits[f]);
    if (FIELDS[f].kind == K_LIST) {
        Json *list = list_from_text(text); free(text);
        Str out; str_init(&out);
        for (size_t i = 0; i < json_count(list); i++) { if (out.len) str_appendc(&out, '\n'); str_appendz(&out, json_str(json_at(list, i))); }
        json_free(list);
        return out.data ? str_detach(&out) : xstrdup("");
    }
    char *t = str_trim(text); free(text);
    return t;
}

/// The body a save sends: the row the form came from with every field as it stands now. NULL with `*why` when a field
/// cannot be sent as it is; a NULL `why` takes the form as it stands.
static Json *form_body(FormScreen *s, char **why, int *tab) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    // What the server sets itself; the login never leaves it, and the order belongs to the list.
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder", "hasLogin", "loginDir" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    json_set_str(body, "binary", s->binary);
    json_set_bool(body, "active", s->active);
    for (int f = 0; f < F_COUNT; f++) {
        if (!FIELDS[f].key) continue;
        char *v = field_now(s, f);
        if (FIELDS[f].kind == K_LIST) json_object_set(body, FIELDS[f].key, list_from_text(v));
        else json_set_str(body, FIELDS[f].key, v);
        free(v);
    }
    if (why && str_empty(json_str(json_get(body, "label")))) {
        *why = xstrdup("Enter a label: it is what the provider picker shows.");
        *tab = T_PROVIDER;
        json_free(body);
        return NULL;
    }
    return body;
}

static bool tab_changed(FormScreen *s, int t);
static void form_changed(FormScreen *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = tab_changed(s, g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

static bool tab_changed(FormScreen *s, int t) {
    if (t == T_PROVIDER) {
        if (s->active != !json_bool_is(json_get(s->row, "active"), false) || !str_eq(s->binary, row_binary(s->row))) return true;
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (field_tab(f) != t || !FIELDS[f].key) continue;
        char *now = field_now(s, f), *saved = field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}
static int open_tab(FormScreen *s) { return g_tab == T_STATUS && !s->id ? T_PROVIDER : g_tab; }
static void layout_tabs(FormScreen *s, Doc *doc, int x, int w) {
    int h = px(42), tx = x, ty = doc->y, open = open_tab(s);
    for (int t = 0; t < T_COUNT; t++) {
        if (t == T_STATUS && !s->id) continue;
        bool changed = s->dirty && tab_changed(s, t);
        if (t == open) s->tab_dot = changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", TABS[t].title) : xstrdup(TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + w, h, TABS[t].glyph, title, NULL, t == open, ACT_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(18));
}

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}
static void hint(Doc *doc, int x, int w, const char *text) {
    doc_rich(doc, x, w, text, FONT_CAPTION, theme.muted);
}
/// A labelled box with its edit, and the hint under it; advances.
static void field(FormScreen *s, Doc *doc, int x, int w, int f, const char *note) {
    if (!field_shown(s, f)) return;
    const FieldDef *d = &FIELDS[f];
    doc_text(doc, x, w, d->label, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = font_height(doc->cv, fid);
    int h = is_multiline(f) ? shown_rows(s, f) * fh + px(16) : px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = is_multiline(f) ? (RECT){ x + px(10), box.top + px(8), x + w - px(3), box.bottom - px(8) }
                                  : (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    if (note) { doc_space(doc, px(6)); hint(doc, x, w, note); }
    doc_space(doc, px(14));
}
/// Two fields side by side, as the dashboard's `.field-row`; advances past the taller.
static void field_pair(FormScreen *s, Doc *doc, int x, int w, int a, const char *note_a, int b, const char *note_b) {
    int gap = px(14), half = (w - gap) / 2, top = doc->y;
    field(s, doc, x, half, a, note_a);
    int left_bottom = doc->y;
    doc->y = top;
    field(s, doc, x + half + gap, w - half - gap, b, note_b);
    if (doc->y < left_bottom) doc->y = left_bottom;
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FormScreen *s = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), s->active ? theme.accent : theme.field, s->active ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->active) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, "Active: new sessions may start on this provider", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

typedef struct { char *text; } SelectData;
static void select_free(void *p) { SelectData *d = p; free(d->text); free(d); }
static void paint_select(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SelectData *d = it->data;
    bool hovered = doc->hover >= 0 && doc_item(doc, doc->hover) == it;
    fill_round_rect(cv, rc, px(6), theme.raise, hovered ? theme.accent_dim : theme.line);
    RECT t = { rc->left + px(12), rc->top, rc->right - px(28), rc->bottom };
    draw_text(cv, d->text, &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT c = { rc->right - px(26), rc->top, rc->right - px(10), rc->bottom };
    draw_glyph(cv, 0xE70D, &c, FONT_ICON_SMALL, theme.ink);
}
/// A labelled select that opens a menu; advances.
static void select_box(Doc *doc, int x, int w, const char *label, const char *text, int action) {
    doc_text(doc, x, w, label, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(6));
    RECT box = { x, doc->y, x + w, doc->y + px(36) };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_select));
    SelectData *d = xcalloc(1, sizeof *d); d->text = xstrdup(text);
    it->data = d; it->free_data = select_free; it->action = action; it->hand = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}
static void note(Doc *doc, int x, int w, const char *text) {
    hint(doc, x, w, text);
    doc_space(doc, px(12));
}

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
typedef struct { char *text; const char *dot; } HeadlineData;
static void headline_free(void *p) { HeadlineData *d = p; free(d->text); free(d); }
static void paint_headline(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    HeadlineData *d = it->data;
    int cy = (rc->top + rc->bottom) / 2;
    draw_status_dot(cv, rc->left + px(4), cy, d->dot);
    RECT t = { rc->left + px(16), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_SUBHEADLINE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void headline(FormScreen *s, Doc *doc, int x, int w) {
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
typedef struct { char *label, *right; double pct; } BarData;
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
static void layout_status(FormScreen *s, Doc *doc, int x, int w) {
    headline(s, doc, x, w);
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
    note(doc, x, w, "Check usage reads the account and its quota again rather than the server's last answer.");
}

/// The Connection tab: the mode, then the login, or the endpoint with its token and Test.
static void layout_connection(FormScreen *s, Doc *doc, int x, int w) {
    if (mode_offered(s->binary)) select_box(doc, x, (w - px(14)) / 2, "Mode", s->token ? "API token" : "Login", ACT_MODE);
    else if (str_eq(s->binary, "grok")) note(doc, x, w, "grok signs in with its own login; it takes no API token.");
    else note(doc, x, w, "opencode authenticates with an API key per service and has no login of its own.");
    if (!uses_token(s)) {
        if (!s->id) { note(doc, x, w, "Save the provider first: its login is registered against the saved entry, so there is nothing to log in until one exists."); return; }
        headline(s, doc, x, w);
        doc_space(doc, px(12));
        bool supported = store_supports(str_eq(s->binary, "claude") ? "provider_login_start" : "provider_login");
        doc_button(doc, x, 0, s->req_login ? "Logging in\xE2\x80\xA6" : "Log in", BUTTON_BORDERED, ACT_LOGIN, 0, supported && !s->req_login);
        doc_space(doc, px(10));
        if (s->device_code) {
            // codex's confirm page asks for the code its hidden CLI printed.
            doc_text(doc, x, w, "Enter this code on the page that opened; the login is picked up automatically.", FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
            doc_space(doc, px(6));
            int top = doc->y;
            doc_text(doc, x, w, s->device_code, FONT_MONO, theme.ink, DT_LEFT | DT_SINGLELINE);
            int cw = text_width(doc->cv, s->device_code, FONT_MONO);
            doc->y = top;
            doc_button(doc, x + cw + px(14), 0, "Copy", BUTTON_PLAIN, ACT_COPY_CODE, 0, true);
            doc_space(doc, px(12));
        }
        if (field_shown(s, F_LOGIN_CODE)) {
            note(doc, x, w, "Approve in the browser, then paste the code it shows here.");
            field(s, doc, x, w, F_LOGIN_CODE, NULL);
            doc_button(doc, x, 0, "Finish", BUTTON_PROMINENT, ACT_FINISH, 0, !s->req_login && store_supports("provider_login_finish"));
            doc_space(doc, px(12));
            return;   // the hint above the code says it already
        }
        note(doc, x, w, str_eq(s->binary, "claude")
            ? "The login happens in the browser: approve on claude.ai, then paste the code it shows back here."
            : "The login happens in the browser: the server runs the CLI's device login and picks it up once approved.");
        return;
    }
    field(s, doc, x, w, F_BASE_URL, base_url_hint(s->binary));
    field(s, doc, x, w, F_API_KEY, api_key_hint(s->binary));
    doc_button(doc, x, 0, s->req_test ? "Testing\xE2\x80\xA6" : "Test", BUTTON_BORDERED, ACT_TEST, 0, !s->req_test && store_supports("test_provider"));
    doc_space(doc, px(8));
    if (s->test_result && !s->req_test) { doc_text(doc, x, w, s->test_result, FONT_FOOTNOTE, s->test_failed ? theme.danger : theme.ok, DT_LEFT | DT_WORDBREAK); doc_space(doc, px(8)); }
    note(doc, x, w, "Test probes the endpoint and token as the form holds them, no save needed, and puts the endpoint's models in the Models tab.");
}

static void form_layout(Screen *base, Doc *doc) {
    FormScreen *s = (FormScreen *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    if (!store_supports("settings_providers")) {
        doc_space(doc, px(10));
        doc_text(doc, x, col, "Provider settings need an Admin token on a server that offers them (GET /settings/providers).", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        return;
    }
    layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (open_tab(s)) {
    case T_PROVIDER: {
        doc_item(doc, doc_custom(doc, x, col, px(26), paint_check, s, NULL, ACT_ACTIVE, 0))->hand = true;
        doc_space(doc, px(12));
        int gap = px(14), half = (col - gap) / 2, top = doc->y;
        field(s, doc, x, half, F_LABEL, NULL);
        int left_bottom = doc->y;
        doc->y = top;
        select_box(doc, x + half + gap, col - half - gap, "Binary", binary_title(s->binary), ACT_BINARY);
        if (doc->y < left_bottom) doc->y = left_bottom;
        note(doc, x, col, "A provider is one login or endpoint of one CLI. Its mode, login and endpoint are under Connection.");
        break;
    }
    case T_CONNECTION: layout_connection(s, doc, x, col); break;
    case T_MODELS:
        field_pair(s, doc, x, col, F_MODELS, "One model per line; empty offers the CLI's own list.", F_EFFORTS, "One effort per line; empty offers the CLI's own.");
        field_pair(s, doc, x, col, F_DEFAULT_MODEL, NULL, F_DEFAULT_EFFORT, NULL);
        break;
    case T_STATUS: layout_status(s, doc, x, col); break;
    }
    doc_space(doc, px(40));
}

static void form_header(Screen *base, HeaderInfo *info) {
    FormScreen *s = (FormScreen *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : "Provider");
        snprintf(info->subtitle, sizeof info->subtitle, "runs the %s CLI", row_binary(s->row));
    } else {
        snprintf(info->title, sizeof info->title, "New provider");
        snprintf(info->subtitle, sizeof info->subtitle, "A provider is a login or endpoint sessions can be started on.");
    }
    if (!store_supports("settings_providers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this provider (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_provider" : "create_provider");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_provider"); c->tip = "Clone into a new provider";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_provider"); d->tip = "Delete this provider";
}

// MARK: The edits

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }
static void form_place(Screen *base, const RECT *content, int scroll_y) {
    FormScreen *s = (FormScreen *)base;
    int m = margin_of(base->pane);
    for (int f = 0; f < F_COUNT; f++) {
        HWND e = s->edits[f];
        if (!e) continue;
        if (!s->shown || !s->laid[f]) { ShowWindow(e, SW_HIDE); continue; }
        RECT r = { content->left + m + s->rects[f].left, content->top + s->rects[f].top - scroll_y, content->left + m + s->rects[f].right, content->top + s->rects[f].bottom - scroll_y };
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) { ShowWindow(e, SW_HIDE); continue; }
        MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        bool clipped = !EqualRect(&visible, &r);
        if (clipped) SetWindowRgn(e, CreateRectRgn(visible.left - r.left, visible.top - r.top, visible.right - r.left, visible.bottom - r.top), TRUE);
        else if (s->clipped[f]) SetWindowRgn(e, NULL, TRUE);
        s->clipped[f] = clipped;
        ShowWindow(e, SW_SHOWNA);
    }
}

static void form_save(FormScreen *s);
static void login_finish(FormScreen *s);
static int next_field(FormScreen *s, int from, int step) {
    for (int k = 1; k <= F_COUNT; k++) {
        int f = ((from + step * k) % F_COUNT + F_COUNT) % F_COUNT;
        if (s->edits[f] && s->laid[f]) return f;
    }
    return from;
}
static LRESULT CALLBACK field_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    FormScreen *s = (FormScreen *)ref;
    int f = (int)id;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_TAB) { SetFocus(s->edits[next_field(s, f, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1)]); return 0; }
        if (ctrl && wp == 'S') { form_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        // Enter in the login code finishes the login; elsewhere it moves on.
        if (wp == VK_RETURN && f == F_LOGIN_CODE) { login_finish(s); return 0; }
        if (wp == VK_RETURN && !is_multiline(f)) { SetFocus(s->edits[next_field(s, f, 1)]); return 0; }
        break;
    case WM_CHAR:
        if (wp == '\t' || wp == 0x13 || wp == 0x01 || wp == 0x1B || (wp == '\r' && !is_multiline(f))) return 0;
        break;
    case WM_MOUSEWHEEL: {
        bool own = is_multiline(f) && SendMessageW(hwnd, EM_GETLINECOUNT, 0, 0) > shown_rows(s, f);
        if (!own) { SendMessageW(GetParent(hwnd), msg, wp, lp); return 0; }
        break;
    }
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, field_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
static void form_ensure_controls(FormScreen *s) {
    if (s->edits[F_LABEL]) return;
    HWND owner = pane_hwnd(s->base.pane);
    for (int f = 0; f < F_COUNT; f++) {
        DWORD style = WS_CHILD | WS_TABSTOP | (is_multiline(f) ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL) | (FIELDS[f].secret ? ES_PASSWORD : 0);
        HWND e = CreateWindowExW(0, L"EDIT", L"", style, 0, 0, 10, 10, owner, (HMENU)(INT_PTR)(ID_FIELD + f), GetModuleHandleW(NULL), NULL);
        SendMessageW(e, WM_SETFONT, (WPARAM)font(FIELDS[f].mono ? FONT_MONO : FONT_BODY), TRUE);
        SendMessageW(e, EM_SETLIMITTEXT, 0, 0);
        if (FIELDS[f].cue) { wchar_t *w = utf8_to_wide(FIELDS[f].cue); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
        SetWindowSubclass(e, field_proc, (UINT_PTR)f, (DWORD_PTR)s);
        theme_apply_control(e);
        s->edits[f] = e;
    }
    form_fill(s);
}

static int popup_menu(FormScreen *s, HMENU menu, POINT pt) {
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}
static void append_item(HMENU menu, UINT id, const char *text, bool checked) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), id, w);
    free(w);
}
static void pick_binary(FormScreen *s, POINT pt) {
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < BINARY_COUNT; i++) append_item(m, (UINT)i + 1, BINARIES[i].title, str_eq(BINARIES[i].id, s->binary));
    int chosen = popup_menu(s, m, pt);
    if (chosen < 1 || chosen > BINARY_COUNT || str_eq(BINARIES[chosen - 1].id, s->binary)) return;
    set_string(&s->binary, BINARIES[chosen - 1].id);
    // Another CLI's login flow and probe verdict say nothing about this one.
    set_string(&s->test_result, NULL); set_string(&s->device_code, NULL); s->code_wanted = false;
    form_changed(s);
    pane_relayout(s->base.pane);
}
static void pick_mode(FormScreen *s, POINT pt) {
    HMENU m = CreatePopupMenu();
    append_item(m, 1, "Login", !s->token);
    append_item(m, 2, "API token", s->token);
    int chosen = popup_menu(s, m, pt);
    if (!chosen || (chosen == 2) == s->token) return;
    // Switching to Login drops the endpoint and its token when saved; switching back brings back what the boxes hold.
    s->token = chosen == 2;
    set_string(&s->test_result, NULL);
    form_changed(s);
    pane_relayout(s->base.pane);
}

// MARK: Status, login and Test

static void status_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->status_error, t); free(t); }
    else {
        json_free(s->status); s->status = json_clone(json_get(req->result, "status"));
        set_string(&s->status_error, NULL);
        // A login that landed shows in the sidebar's own login tag.
        if (json_bool_is(json_get(json_get(s->status, "auth"), "loggedIn"), true) && s->device_code) { set_string(&s->device_code, NULL); settings_providers_changed(false); }
    }
    pane_relayout(s->base.pane);
}
static void load_status(FormScreen *s, bool fresh) {
    if (!s->id || !store_supports("provider_status")) return;
    request_cancel(&s->req_status);
    Json *args = json_object(); json_set_num(args, "id", s->id);
    if (fresh) json_set_num(args, "fresh", 1);
    s->status_fresh = fresh;
    store_call("provider_status", args, 0, s, status_done, 0, &s->req_status);
    pane_relayout(s->base.pane);
}

static void login_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->error, t); free(t); pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    set_string(&s->error, NULL);
    const char *url = json_str_nonempty(json_get(req->result, "url"));
    if (str_eq(s->binary, "claude")) {
        // claude.ai shows a code once approved; it comes back here to finish.
        if (url) { open_web_url(url); s->code_wanted = true; s->focus_first = F_LOGIN_CODE; }
    } else if (!url) {
        app_alert("Already logged in", "This entry is already logged in.");
        load_status(s, false);
    } else {
        // Device auth prints the URL and waits, so the app opens it; the status is read again a few times to catch the login.
        open_web_url(url);
        const char *code = json_str_nonempty(json_get(req->result, "deviceCode"));
        set_string(&s->device_code, code);
        s->login_wait = 0;
        SetTimer(pane_hwnd(s->base.pane), TIMER_LOGIN, (UINT)LOGIN_WAITS_MS[0], NULL);
    }
    pane_relayout(s->base.pane);
    if (s->focus_first >= 0 && s->edits[s->focus_first]) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
}
static void login_start(FormScreen *s) {
    if (!can_log_in(s) || s->req_login) return;
    const char *op = str_eq(s->binary, "claude") ? "provider_login_start" : "provider_login";
    if (!store_supports(op)) return;
    set_string(&s->device_code, NULL); s->code_wanted = false;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call(op, args, 0, s, login_done, 0, &s->req_login);
    pane_relayout(s->base.pane);
}
static void finish_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "provider") : NULL;
    if (!json_is_object(row)) {
        char *t = req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
        set_string(&s->error, t); free(t);
        pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    set_string(&s->error, NULL);
    s->code_wanted = false;
    if (s->edits[F_LOGIN_CODE]) set_edit_text(s->edits[F_LOGIN_CODE], "");
    // The saved row now carries its login; the form keeps any change not saved yet.
    json_set_bool(s->row, "hasLogin", true);
    const char *dir = json_str(json_get(row, "loginDir"));
    if (dir) json_set_str(s->row, "loginDir", dir);
    load_status(s, false);
    settings_providers_changed(false);
    const char *label = json_str_nonempty(json_get(row, "label"));
    char *message = xstrfmt("%s is logged in.", label ? label : "The provider");
    app_alert("Logged in", message);
    free(message);
}
static void login_finish(FormScreen *s) {
    if (!s->code_wanted || s->req_login || !s->edits[F_LOGIN_CODE] || !store_supports("provider_login_finish")) return;
    char *text = edit_text(s->edits[F_LOGIN_CODE]), *code = str_trim(text); free(text);
    if (!*code) { set_string(&s->error, "Paste the code from the browser first."); free(code); pane_relayout(s->base.pane); return; }
    Json *args = json_object(); json_set_num(args, "id", s->id); json_set_str(args, "code", code);
    free(code);
    store_call("provider_login_finish", args, 0, s, finish_done, 0, &s->req_login);
    pane_relayout(s->base.pane);
}

static void test_done(void *owner, Request *req) {
    FormScreen *s = owner;
    s->test_failed = !req->ok;
    if (!req->ok) { char *t = request_error_text(req); set_string(&s->test_result, t); free(t); pane_relayout(s->base.pane); return; }
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
    char *now = s->edits[F_MODELS] ? field_now(s, F_MODELS) : xstrdup("");
    bool same = str_eq(now, joined);
    if (!same && s->edits[F_MODELS]) { set_edit_text(s->edits[F_MODELS], joined); s->lines[F_MODELS] = line_count(joined); }
    char *t = xstrfmt("OK: the endpoint offers %zu model%s%s", n, n == 1 ? "" : "s", same ? "; the Models tab already matches" : "; they are in the Models tab, save to keep them");
    set_string(&s->test_result, t);
    free(t); free(now); free(joined);
    pane_relayout(s->base.pane);
}
static void test_endpoint(FormScreen *s) {
    if (s->req_test || !store_supports("test_provider")) return;
    Json *args = json_object();
    json_set_str(args, "binary", s->binary);
    char *v = field_now(s, F_BASE_URL); json_set_str(args, "baseUrl", v); free(v);
    v = s->edits[F_API_KEY] ? edit_text(s->edits[F_API_KEY]) : xstrdup(""); json_set_str(args, "apiKey", v); free(v);
    v = field_now(s, F_DEFAULT_MODEL); json_set_str(args, "defaultModel", v); free(v);
    v = field_now(s, F_MODELS); json_object_set(args, "models", list_from_text(v)); free(v);
    // The saved row's id lets the server probe as the model it resolves for it.
    if (s->id) json_set_num(args, "id", s->id);
    set_string(&s->test_result, "Testing\xE2\x80\xA6"); s->test_failed = false;
    store_call("test_provider", args, 0, s, test_done, 0, &s->req_test);
    pane_relayout(s->base.pane);
}

// MARK: Saving, cloning, deleting

static void form_set_id(FormScreen *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = form_id(id);
    pane_set_selected_id(app_sidebar_pane(), s->base.id);
}
static void save_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "provider") : NULL;
    if (!json_is_object(row)) {
        char *text = req->ok ? xstrdup("The server returned an unexpected response.") : request_error_text(req);
        set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    bool was_new = !s->id;
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, row_id(row));
    form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_providers_changed(false);
    // A new provider's connection can be read now that it exists.
    if (was_new || !s->status) load_status(s, false);
}
static void form_save(FormScreen *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_provider" : "create_provider")) return;
    char *why = NULL; int tab = g_tab;
    Json *body = form_body(s, &why, &tab);
    if (!body) { set_string(&s->error, why); free(why); g_tab = tab; pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_provider" : "create_provider", body, 0, s, save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}
static void delete_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) {
        char *text = request_error_text(req); set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    s->dirty = false;
    app_clear_detail();
    settings_providers_changed(true);
}
static void form_delete(FormScreen *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this provider");
    bool ok = app_confirm(title, "Sessions already run on it keep their history, but no new one can be started on it.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_provider", args, 0, s, delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}
static void form_clone(FormScreen *s) {
    // The copy carries what the form holds now, saved or not, without its label, which is typed first.
    int tab = g_tab;
    Json *copy = form_body(s, NULL, &tab);
    json_set_str(copy, "label", "");
    s->dirty = false;
    Screen *clone = provider_settings_screen_new(copy, NULL);
    ((FormScreen *)clone)->focus_first = F_LABEL;
    g_tab = T_PROVIDER;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void form_destroy(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_LOGIN);
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_status);
    request_cancel(&s->req_login); request_cancel(&s->req_test);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->row); json_free(s->status);
    free(s->binary); free(s->status_error); free(s->test_result); free(s->device_code); free(s->error);
    screen_release(base);
}
static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    FormScreen *s = (FormScreen *)base;
    switch (action) {
    case ACT_SAVE: form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_ACTIVE: s->active = !s->active; form_changed(s); pane_relayout(base->pane); break;
    case ACT_BINARY: pick_binary(s, pt); break;
    case ACT_MODE: pick_mode(s, pt); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_LOGIN: login_start(s); break;
    case ACT_FINISH: login_finish(s); break;
    case ACT_TEST: test_endpoint(s); break;
    case ACT_CHECK: load_status(s, true); break;
    case ACT_COPY_CODE: if (s->device_code) copy_to_clipboard(pane_hwnd(base->pane), s->device_code); break;
    }
}
static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    FormScreen *s = (FormScreen *)base;
    int f = id - ID_FIELD;
    if (f < 0 || f >= F_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE:
        if (f == F_LOGIN_CODE) break;
        form_changed(s);
        if (is_multiline(f)) {
            char *text = edit_text(s->edits[f]);
            int n = line_count(text);
            free(text);
            if (n != s->lines[f]) { s->lines[f] = n; pane_relayout(base->pane); }
        }
        break;
    case EN_SETFOCUS: {
        s->focused = f;
        RECT content = pane_content_rect(base->pane);
        int top = s->rects[f].top - px(40), bottom = s->rects[f].bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: if (s->focused == f) s->focused = -1; pane_repaint(base->pane); break;
    }
}
static void form_timer(Screen *base, UINT id) {
    FormScreen *s = (FormScreen *)base;
    if (id != TIMER_LOGIN) return;
    KillTimer(pane_hwnd(base->pane), TIMER_LOGIN);
    load_status(s, false);
    int n = (int)(sizeof LOGIN_WAITS_MS / sizeof *LOGIN_WAITS_MS);
    if (++s->login_wait < n && s->device_code) SetTimer(pane_hwnd(base->pane), TIMER_LOGIN, (UINT)(LOGIN_WAITS_MS[s->login_wait] - LOGIN_WAITS_MS[s->login_wait - 1]), NULL);
}
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { form_save((FormScreen *)base); return true; }
    return false;
}
static void form_refresh(Screen *base) { load_status((FormScreen *)base, false); }
static void form_visible(Screen *base, bool shown) {
    FormScreen *s = (FormScreen *)base;
    s->shown = shown;
    if (shown) {
        form_ensure_controls(s);
        if (s->id && !s->status && !s->req_status) load_status(s, false);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}
static bool form_can_leave(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this provider") : xstrdup("The new provider has not been saved.");
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = form_layout, .header = form_header, .action = form_action, .place = form_place,
    .visible = form_visible, .command = form_command, .key = form_key, .timer = form_timer, .refresh = form_refresh,
    .can_leave = form_can_leave,
};
Screen *provider_settings_screen_new(const Json *row, const Json *defaults) {
    FormScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &form_vt;
    s->focused = -1; s->focus_first = -1;
    // A saved row, a clone's values without an id, or a new provider from the server's defaults.
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = row_id(s->row);
    s->base.id = form_id(s->id);
    if (!s->id) { s->focus_first = F_LABEL; g_tab = T_PROVIDER; }
    form_fill(s);
    return &s->base;
}
