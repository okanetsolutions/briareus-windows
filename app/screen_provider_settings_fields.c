#include "screen_provider_settings_internal.h"

static int field_tab(int f);
static char *field_text(const Json *row, int f);

static int field_tab(int f) {
    switch (f) {
    case F_LABEL: case F_BASE_URL: case F_API_KEY: case F_LOGIN_CODE: return T_PROVIDER;
    default: return T_MODELS;
    }
}

int screen_provider_settings_line_count(const char *text) { int n = 1; for (const char *c = text; c && *c; c++) if (*c == '\n') n++; return n; }

bool screen_provider_settings_is_multiline(int f) { return screen_provider_settings_FIELDS[f].kind == K_LIST; }

int screen_provider_settings_shown_rows(FormScreen *s, int f) { int n = s->lines[f] > screen_provider_settings_FIELDS[f].rows ? s->lines[f] : screen_provider_settings_FIELDS[f].rows; return n < MAX_ROWS ? n : MAX_ROWS; }

// MARK: What the binary and the mode allow

bool screen_provider_settings_field_shown(FormScreen *s, int f) {
    if (f == F_BASE_URL || f == F_API_KEY) return screen_provider_settings_uses_token(s);
    if (f == F_LOGIN_CODE) return s->code_wanted && screen_provider_settings_can_log_in(s);
    return true;
}

// MARK: Reading and writing the fields

char *screen_provider_settings_edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}

void screen_provider_settings_set_edit_text(HWND edit, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(edit, w);
    free(w); free(crlf);
}

/// A field's value in the row, as the edit shows it: a list one item per line.
static char *field_text(const Json *row, int f) {
    if (!screen_provider_settings_FIELDS[f].key) return xstrdup("");
    const Json *v = json_get(row, screen_provider_settings_FIELDS[f].key);
    if (screen_provider_settings_FIELDS[f].kind == K_LIST) {
        Str out; str_init(&out);
        for (size_t i = 0; i < json_count(v); i++) { const char *line = json_str(json_at(v, i)); if (!line) continue; if (out.len) str_appendc(&out, '\n'); str_appendz(&out, line); }
        return out.data ? str_detach(&out) : xstrdup("");
    }
    return xstrdup(json_str(v) ? json_str(v) : "");
}

Json *screen_provider_settings_list_from_text(const char *text) {
    Json *list = json_array();
    size_t n; char **lines = str_split(text, '\n', &n);
    for (size_t i = 0; i < n; i++) { char *t = str_trim(lines[i]); if (*t) json_array_push(list, json_string(t)); free(t); }
    str_array_free(lines, n);
    return list;
}

const char *screen_provider_settings_row_binary(const Json *row) { const char *b = json_str_nonempty(json_get(row, "binary")); return b ? b : "claude"; }

/// Fills every field from `s->row`: the edits once they exist, the flags and the mode at once. The mode is not stored: a
/// row with a token or an endpoint is in API token mode, every other one logs in.
void screen_provider_settings_form_fill(FormScreen *s) {
    s->active = !json_bool_is(json_get(s->row, "active"), false);
    set_string(&s->binary, screen_provider_settings_row_binary(s->row));
    s->token = json_str_nonempty(json_get(s->row, "apiKey")) || json_str_nonempty(json_get(s->row, "baseUrl"));
    s->filling = true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f] || !screen_provider_settings_FIELDS[f].key) continue;
        char *text = field_text(s->row, f);
        screen_provider_settings_set_edit_text(s->edits[f], text);
        s->lines[f] = screen_provider_settings_line_count(text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
}

/// A field's value as the form would send it, as a new string: trimmed, and the endpoint's emptied when the mode drops it.
char *screen_provider_settings_field_now(FormScreen *s, int f) {
    if ((f == F_BASE_URL || f == F_API_KEY) && !screen_provider_settings_uses_token(s)) return xstrdup("");
    if (!s->edits[f]) return field_text(s->row, f);
    char *text = screen_provider_settings_edit_text(s->edits[f]);
    if (screen_provider_settings_FIELDS[f].kind == K_LIST) {
        Json *list = screen_provider_settings_list_from_text(text); free(text);
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
Json *screen_provider_settings_form_body(FormScreen *s, char **why, int *tab) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    // What the server sets itself; the login never leaves it, and the order belongs to the list.
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder", "hasLogin", "loginDir" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    json_set_str(body, "binary", s->binary);
    json_set_bool(body, "active", s->active);
    for (int f = 0; f < F_COUNT; f++) {
        if (!screen_provider_settings_FIELDS[f].key) continue;
        char *v = screen_provider_settings_field_now(s, f);
        if (screen_provider_settings_FIELDS[f].kind == K_LIST) json_object_set(body, screen_provider_settings_FIELDS[f].key, screen_provider_settings_list_from_text(v));
        else json_set_str(body, screen_provider_settings_FIELDS[f].key, v);
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

void screen_provider_settings_form_changed(FormScreen *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = screen_provider_settings_tab_changed(s, screen_provider_settings_g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Layout

bool screen_provider_settings_tab_changed(FormScreen *s, int t) {
    if (t == T_PROVIDER) {
        if (s->active != !json_bool_is(json_get(s->row, "active"), false) || !str_eq(s->binary, screen_provider_settings_row_binary(s->row))) return true;
    }
    for (int f = 0; f < F_COUNT; f++) {
        if (field_tab(f) != t || !screen_provider_settings_FIELDS[f].key) continue;
        char *now = screen_provider_settings_field_now(s, f), *saved = field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}
