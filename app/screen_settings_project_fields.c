#include "screen_settings_internal.h"

static void pick_set(RuntimePick *p, int provider_id, const char *model, const char *effort);
static void read_picks(FormScreen *s);
static void runtimes_done(void *owner, Request *req);
static int popup(FormScreen *s, HMENU menu, POINT pt);

int screen_settings_field_tab(int f) {
    // The project and how a checkout of it is set up, on one tab.
    if (f <= F_PHP) return T_PROJECT;
    if (f <= F_DB_RESTORE) return T_DATABASE;
    if (f <= F_FEEDBACK_STEPS) return T_REVIEW;
    if (f <= F_IS_SELF) return T_ORCHESTRATOR;
    if (f == F_ENV) return T_ENV;
    return T_RUN;
}

int screen_settings_runtime_tab(int r) { return r == R_WORKER ? T_ORCHESTRATOR : T_REVIEW; }

int screen_settings_line_count(const char *text) { int n = 1; for (const char *c = text; c && *c; c++) if (*c == '\n') n++; return n; }

bool screen_settings_is_edit(int f) { return screen_settings_FIELDS[f].kind != K_BOOL; }

bool screen_settings_is_multiline(int f) { return screen_settings_FIELDS[f].kind == K_LIST || screen_settings_FIELDS[f].kind == K_AREA; }

/// The lines a long box shows: its own number, or as many as it holds up to MAX_ROWS.
int screen_settings_shown_rows(FormScreen *s, int f) { int n = s->lines[f] > screen_settings_FIELDS[f].rows ? s->lines[f] : screen_settings_FIELDS[f].rows; return n < MAX_ROWS ? n : MAX_ROWS; }

/// Whether the row has this key. A server that dropped a setting, or predates one, gets neither the box nor the key.
bool screen_settings_row_has(FormScreen *s, const char *key) {
    if (!json_count(s->row)) return true;
    for (size_t i = 0; i < json_count(s->row); i++) if (str_eq(json_key(s->row, i), key)) return true;
    return false;
}

bool screen_settings_field_offered(FormScreen *s, int f) { return f == F_BABYSIT ? s->babysit.supported : screen_settings_row_has(s, screen_settings_FIELDS[f].key); }
bool screen_settings_field_bool(FormScreen *s, int f) { return f == F_BABYSIT ? s->babysit.on : s->bools[f]; }

bool screen_settings_field_enabled(FormScreen *s, int f) { return f != F_DB_RESTORE || s->bools[F_DB_POOL]; }

void screen_settings_pick_clear(RuntimePick *p) { free(p->model); free(p->effort); memset(p, 0, sizeof *p); }

static void pick_set(RuntimePick *p, int provider_id, const char *model, const char *effort) {
    char *m = xstrdup(model), *e = xstrdup(effort);
    screen_settings_pick_clear(p);
    p->provider_id = provider_id; p->model = m; p->effort = e;
}

// MARK: Reading and writing the fields

char *screen_settings_edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    char *lf = str_replace(text, "\r\n", "\n"); free(text);
    return lf;
}

void screen_settings_set_edit_text(HWND edit, const char *text) {
    char *crlf = str_replace(text ? text : "", "\n", "\r\n");
    wchar_t *w = utf8_to_wide(crlf);
    SetWindowTextW(edit, w);
    free(w); free(crlf);
}

/// A field's value in the row, as the edit shows it: a list one item per line, a number as typed, empty for null.
char *screen_settings_field_text(const Json *row, int f) {
    const Json *v = json_get(row, screen_settings_FIELDS[f].key);
    if (screen_settings_FIELDS[f].kind == K_LIST) {
        Str out; str_init(&out);
        for (size_t i = 0; i < json_count(v); i++) { const char *line = json_str(json_at(v, i)); if (!line) continue; if (out.len) str_appendc(&out, '\n'); str_appendz(&out, line); }
        return out.data ? str_detach(&out) : xstrdup("");
    }
    double n;
    if (screen_settings_FIELDS[f].kind == K_NUMBER) return json_num(v, &n) && isfinite(n) ? xstrfmt("%.10g", n) : xstrdup("");
    if (screen_settings_FIELDS[f].kind == K_BOARD) { char *url = project_board_setting_url(v); return url ? url : xstrdup(""); }
    return xstrdup(json_str(v) ? json_str(v) : "");
}

/// A picker's runtime as the row has it saved.
void screen_settings_row_pick(FormScreen *s, int r, RuntimePick *out) {
    const RuntimeDef *d = &screen_settings_RUNTIMES[r];
    const Json *src = d->step ? json_get(json_get(s->row, "stepRuntimes"), d->step) : s->row;
    const char *pk = d->step ? "providerId" : d->provider_key, *mk = d->step ? "model" : d->model_key, *ek = d->step ? "effort" : d->effort_key;
    pick_set(out, json_int_or(json_get(src, pk), 0), json_str(json_get(src, mk)), json_str(json_get(src, ek)));
}

static void read_picks(FormScreen *s) { for (int r = 0; r < R_COUNT; r++) screen_settings_row_pick(s, r, &s->picks[r]); }

/// Fills every field from `s->row`. The edits only once they exist; the flags and pickers at once.
void screen_settings_form_fill(FormScreen *s) {
    for (int f = 0; f < F_COUNT; f++) if (!screen_settings_is_edit(f)) s->bools[f] = json_bool_is(json_get(s->row, screen_settings_FIELDS[f].key), true);
    s->babysit = project_babysit_read(s->row);
    read_picks(s);
    s->filling = true;
    for (int f = 0; f < F_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *text = screen_settings_field_text(s->row, f);
        screen_settings_set_edit_text(s->edits[f], text);
        s->lines[f] = screen_settings_line_count(text);
        free(text);
    }
    s->filling = false;
    s->dirty = false;
}

/// The body a save sends: the row the form came from with every field as it stands now. NULL with `*why` when a field
/// cannot be sent as it is.
Json *screen_settings_form_body(FormScreen *s, char **why, int *tab) {
    Json *body = json_is_object(s->row) ? json_clone(s->row) : json_object();
    // What the server sets itself, and the order, which belongs to the list's Move up and Move down rather than to one project.
    static const char *const server_keys[] = { "id", "createdAt", "updatedAt", "sortOrder" };
    for (size_t i = 0; i < sizeof server_keys / sizeof *server_keys; i++) json_object_remove(body, server_keys[i]);
    for (int f = 0; f < F_COUNT; f++) {
        const char *key = screen_settings_FIELDS[f].key;
        if (f == F_BABYSIT || !screen_settings_field_offered(s, f)) continue;
        if (screen_settings_FIELDS[f].kind == K_BOOL) { json_set_bool(body, key, s->bools[f]); continue; }
        if (!s->edits[f]) continue;
        char *text = screen_settings_edit_text(s->edits[f]);
        switch (screen_settings_FIELDS[f].kind) {
        case K_TEXT: { char *t = str_trim(text); json_set_str(body, key, t); free(t); break; }
        case K_AREA: json_set_str(body, key, text); break;
        case K_LIST: {
            // One item per line, blank lines dropped, as the dashboard reads its textareas.
            Json *list = json_array();
            size_t n; char **lines = str_split(text, '\n', &n);
            for (size_t i = 0; i < n; i++) { char *t = str_trim(lines[i]); if (*t) json_array_push(list, json_string(t)); free(t); }
            str_array_free(lines, n);
            json_object_set(body, key, list);
            break;
        }
        case K_NUMBER: {
            char *t = str_trim(text);
            if (!*t) json_object_set(body, key, json_null());
            else {
                char *end; double n = strtod(t, &end);
                if (*end || !isfinite(n) || n < 0) {
                    *tab = screen_settings_field_tab(f);
                    *why = xstrfmt("%s must be a number of dollars, such as 25 or 12.5, or empty for no cap.", screen_settings_FIELDS[f].label);
                    free(t); free(text); json_free(body);
                    return NULL;
                }
                json_set_num(body, key, n);
            }
            free(t);
            break;
        }
        case K_BOARD: {
            char *t = str_trim(text);
            Json *board = *t ? project_board_setting_from_url(t) : json_null();
            free(t);
            if (!board) {
                *tab = screen_settings_field_tab(f);
                *why = xstrfmt("%s must be the board's address on GitHub, such as https://github.com/orgs/acme/projects/1/views/2, or empty for none.", screen_settings_FIELDS[f].label);
                free(text); json_free(body);
                return NULL;
            }
            json_object_set(body, key, board);
            break;
        }
        case K_BOOL: break;
        }
        free(text);
    }
    project_babysit_write(&s->babysit, body);
    char *repo = str_trim(json_str(json_get(body, "repo")) ? json_str(json_get(body, "repo")) : "");
    bool repo_ok = *repo && strchr(repo, '/') && repo[0] != '/' && repo[strlen(repo) - 1] != '/';
    free(repo);
    if (!repo_ok) { *why = xstrdup("Enter the repository as owner/name."); *tab = T_PROJECT; json_free(body); return NULL; }
    // The runtimes. A step left on "Same as the code review" is no entry at all rather than empty strings.
    bool has_steps = screen_settings_row_has(s, "stepRuntimes");
    Json *steps = json_is_object(json_get(body, "stepRuntimes")) ? json_clone(json_get(body, "stepRuntimes")) : json_object();
    for (int r = 0; r < R_COUNT; r++) {
        const RuntimeDef *d = &screen_settings_RUNTIMES[r];
        const RuntimePick *p = &s->picks[r];
        if (!screen_settings_row_has(s, d->step ? "stepRuntimes" : d->provider_key)) continue;
        if (d->step) {
            if (!p->provider_id) { json_object_remove(steps, d->step); continue; }
            Json *entry = json_object();
            json_set_num(entry, "providerId", p->provider_id);
            json_set_str(entry, "model", p->model ? p->model : "");
            json_set_str(entry, "effort", p->effort ? p->effort : "");
            json_object_set(steps, d->step, entry);
            continue;
        }
        if (p->provider_id) json_set_num(body, d->provider_key, p->provider_id); else json_object_set(body, d->provider_key, json_null());
        json_set_str(body, d->model_key, p->provider_id && p->model ? p->model : "");
        json_set_str(body, d->effort_key, p->provider_id && p->effort ? p->effort : "");
    }
    if (has_steps) json_object_set(body, "stepRuntimes", steps); else json_free(steps);
    return body;
}

void screen_settings_form_changed(FormScreen *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    // The open tab's dot comes and goes as its fields leave and return to what was saved.
    bool changed = screen_settings_tab_changed(s, screen_settings_g_tab);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Runtimes

static void runtimes_done(void *owner, Request *req) {
    FormScreen *s = owner;
    RuntimeCatalog c;
    if (!req->ok || !runtime_catalog_parse(req->result, &c)) return;
    if (s->has_catalog) runtime_catalog_free(&s->catalog);
    s->catalog = c; s->has_catalog = true;
    pane_relayout(s->base.pane);
}

/// The providers the pickers offer. The list is the server's for any project; it is asked with this one, or another.
void screen_settings_load_runtimes(FormScreen *s) {
    if (!store_supports("runtimes")) return;
    const char *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (!s->id || !repo) {
        size_t n; const Project *all = projects_list(&n);
        repo = n ? all[0].repo : NULL;
        if (!repo && screen_settings_g_settings) repo = json_str_nonempty(json_get(json_at(screen_settings_settings_rows(screen_settings_g_settings), 0), "repo"));
    }
    if (!repo) return;
    Json *args = json_object(); json_set_str(args, "repo", repo);
    store_call("runtimes", args, 0, s, runtimes_done, 0, &s->req_runtimes);
}

void screen_settings_append_item(HMENU menu, UINT id, const char *text, bool checked, bool enabled) {
    wchar_t *w = utf8_to_wide(text);
    AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0) | (enabled ? 0 : MF_GRAYED), id, w);
    free(w);
}

static int popup(FormScreen *s, HMENU menu, POINT pt) {
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(s->base.pane), NULL);
    DestroyMenu(menu);
    return chosen;
}

void screen_settings_pick(FormScreen *s, int r, int part, POINT pt) {
    RuntimePick *p = &s->picks[r];
    const RuntimeProvider *pr = s->has_catalog ? runtime_catalog_provider(&s->catalog, p->provider_id) : NULL;
    HMENU m = CreatePopupMenu();
    if (part == 0) {
        screen_settings_append_item(m, 1, screen_settings_RUNTIMES[r].none, p->provider_id == 0, true);
        size_t n = s->has_catalog ? s->catalog.provider_count : 0;
        if (n || (p->provider_id && !pr)) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        for (size_t i = 0; i < n; i++) {
            const RuntimeProvider *o = &s->catalog.providers[i];
            char *label = runtime_provider_available(o) ? xstrdup(o->label) : xstrfmt("%s (unavailable)", o->label);
            screen_settings_append_item(m, (UINT)i + 2, label, o->id == p->provider_id, runtime_provider_available(o) || o->id == p->provider_id);
            free(label);
        }
        // A configured provider the list no longer carries stays picked rather than collapsing to the first choice.
        if (p->provider_id && !pr) { char *label = xstrfmt("Provider #%d (unavailable)", p->provider_id); screen_settings_append_item(m, 9999, label, true, false); free(label); }
        int chosen = popup(s, m, pt);
        if (chosen == 1 && p->provider_id) { screen_settings_pick_clear(p); screen_settings_form_changed(s); }
        else if (chosen >= 2 && (size_t)chosen - 2 < n && s->catalog.providers[chosen - 2].id != p->provider_id) {
            // A new provider starts on its default model and that model's default effort.
            RuntimeChoice c;
            if (runtime_catalog_choice(&s->catalog, s->catalog.providers[chosen - 2].id, NULL, &c)) { pick_set(p, c.provider_id, c.model, c.effort); runtime_choice_free(&c); screen_settings_form_changed(s); }
        }
    } else if (part == 1) {
        size_t n = pr ? pr->model_count : 0;
        for (size_t i = 0; i < n; i++) screen_settings_append_item(m, (UINT)i + 1, runtime_model_title(&pr->models[i]), str_eq(pr->models[i].id, p->model), true);
        if (!n) screen_settings_append_item(m, 9999, s->has_catalog ? "No models listed" : "Loading the providers\xE2\x80\xA6", false, false);
        int chosen = popup(s, m, pt);
        if (chosen >= 1 && (size_t)chosen - 1 < n && !str_eq(pr->models[chosen - 1].id, p->model)) {
            // The effort carries over when the new model offers it, else that model's default.
            const RuntimeModel *model = &pr->models[chosen - 1];
            bool offered = false;
            for (size_t k = 0; k < model->effort_count; k++) if (str_eq(model->efforts[k], p->effort)) offered = true;
            const char *effort = offered ? p->effort : model->default_effort ? model->default_effort : model->effort_count ? model->efforts[0] : "";
            char *keep = xstrdup(effort);
            pick_set(p, p->provider_id, model->id, keep);
            free(keep);
            screen_settings_form_changed(s);
        }
    } else {
        RuntimeChoice c = { p->provider_id, p->model, p->effort };
        size_t n = 0;
        const char *const *efforts = s->has_catalog ? runtime_catalog_efforts(&s->catalog, &c, &n) : NULL;
        for (size_t i = 0; i < n; i++) screen_settings_append_item(m, (UINT)i + 1, efforts[i], str_eq(efforts[i], p->effort), true);
        if (!n) screen_settings_append_item(m, 9999, s->has_catalog ? "This model takes no effort" : "Loading the providers\xE2\x80\xA6", false, false);
        int chosen = popup(s, m, pt);
        if (chosen >= 1 && (size_t)chosen - 1 < n && !str_eq(efforts[chosen - 1], p->effort)) {
            char *effort = xstrdup(efforts[chosen - 1]);
            pick_set(p, p->provider_id, p->model, effort);
            free(effort);
            screen_settings_form_changed(s);
        }
    }
    pane_relayout(s->base.pane);
}
