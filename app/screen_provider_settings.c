#include "screen_provider_settings_internal.h"

static int row_id(const Json *row);
static char *form_id(int id);
static void form_set_id(FormScreen *s, int id);
static void save_done(void *owner, Request *req);
static void delete_done(void *owner, Request *req);
static void form_delete(FormScreen *s);
static void form_clone(FormScreen *s);
static void form_destroy(Screen *base);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static void form_timer(Screen *base, UINT id);
static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void form_refresh(Screen *base);
static void form_visible(Screen *base, bool shown);
static bool form_can_leave(Screen *base);

// One provider's settings, as the dashboard's provider form laid out as the project form is: tabs along the top
// (Provider, Models and, once saved, Status), saved through /settings/providers. A provider is one login or
// endpoint of one CLI; its login happens in the browser and its connection and quota are read from the server.

static int row_id(const Json *row) { return json_int_or(json_get(row, "id"), 0); }

static char *form_id(int id) { return id > 0 ? xstrfmt("settings-provider:%d", id) : xstrdup("settings-provider:new"); }

const FieldDef screen_provider_settings_FIELDS[F_COUNT] = {
    [F_LABEL] = { "label", K_TEXT, "Label", "shown in the provider picker", 0, false, false },
    [F_BASE_URL] = { "baseUrl", K_TEXT, "Base URL", "empty = the binary's own service", 0, true, false },
    [F_API_KEY] = { "apiKey", K_TEXT, "API token", NULL, 0, true, true },
    [F_MODELS] = { "models", K_LIST, "Models", NULL, 4, true, false },
    [F_EFFORTS] = { "efforts", K_LIST, "Efforts", NULL, 4, true, false },
    [F_DEFAULT_MODEL] = { "defaultModel", K_TEXT, "Default model", "empty = the CLI's default", 0, true, false },
    [F_DEFAULT_EFFORT] = { "defaultEffort", K_TEXT, "Default effort", "empty = the CLI's default", 0, true, false },
    [F_LOGIN_CODE] = { NULL, K_TEXT, "Code from the browser", "paste the code from the browser", 0, true, false },
};

/// The open tab stays open from one provider to the next.
int screen_provider_settings_g_tab;

/// After a device login is started, the status is read again this long after it, then after each next wait.
const int screen_provider_settings_LOGIN_WAITS_MS[] = { 10000, 30000, 60000, 120000 };

// MARK: Saving, cloning, deleting

static void form_set_id(FormScreen *s, int id) {
    s->id = id;
    free(s->base.id); s->base.id = form_id(id);
    settings_select_id(s->base.id);
}

static void save_done(void *owner, Request *req) {
    FormScreen *s = owner;
    const Json *row = req->ok ? json_get(req->result, "provider") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        set_string(&s->error, text); free(text);
        pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
        return;
    }
    bool was_new = !s->id;
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    form_set_id(s, row_id(row));
    screen_provider_settings_form_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    settings_providers_changed(false);
    // A new provider's connection can be read now that it exists.
    if (was_new || !s->status) screen_provider_settings_load_status(s, false);
}

void screen_provider_settings_form_save(FormScreen *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_provider" : "create_provider")) return;
    char *why = NULL; int tab = screen_provider_settings_g_tab;
    Json *body = screen_provider_settings_form_body(s, &why, &tab);
    if (!body) { set_string(&s->error, why); free(why); screen_provider_settings_g_tab = tab; pane_relayout(s->base.pane); pane_scroll_to_top(s->base.pane); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_provider" : "create_provider", body, 0, s, save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}

static void delete_done(void *owner, Request *req) {
    FormScreen *s = owner;
    if (!req->ok) {
        request_error_into(&s->error, req);
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
    int tab = screen_provider_settings_g_tab;
    Json *copy = screen_provider_settings_form_body(s, NULL, &tab);
    json_set_str(copy, "label", "");
    s->dirty = false;
    Screen *clone = provider_settings_screen_new(copy, NULL);
    ((FormScreen *)clone)->focus_first = F_LABEL;
    screen_provider_settings_g_tab = T_PROVIDER;
    json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void form_release(FormScreen *s) {
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->binary_ids); json_free(s->row); json_free(s->status); json_free(s->login_from);
    free(s->binary); free(s->status_error); free(s->test_result); free(s->device_code); free(s->error);
    screen_release(&s->base);
}
static void form_destroy(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (s->base.pane) KillTimer(pane_hwnd(s->base.pane), TIMER_LOGIN);
    request_cancel(&s->req_binaries);
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_status);
    request_cancel(&s->req_login); request_cancel(&s->req_test);
    if (s->modal) { s->retired = true; return; }
    form_release(s);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    FormScreen *s = (FormScreen *)base;
    switch (action) {
    case ACT_SAVE: screen_provider_settings_form_save(s); break;
    case ACT_CLONE: form_clone(s); break;
    case ACT_DELETE: form_delete(s); break;
    case ACT_TAB:
        if (arg < 0 || arg >= T_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        screen_provider_settings_g_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    case ACT_ACTIVE: s->active = !s->active; screen_provider_settings_form_changed(s); pane_relayout(base->pane); break;
    case ACT_BINARY: screen_provider_settings_pick_binary(s, pt); break;
    case ACT_MODE: screen_provider_settings_pick_mode(s, pt); break;
    case ACT_FOCUS: if (arg >= 0 && arg < F_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_LOGIN: screen_provider_settings_login_start(s); break;
    case ACT_FINISH: screen_provider_settings_login_finish(s); break;
    case ACT_TEST: screen_provider_settings_test_endpoint(s); break;
    case ACT_CHECK: screen_provider_settings_load_status(s, true); break;
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
        screen_provider_settings_form_changed(s);
        if (screen_provider_settings_is_multiline(f)) {
            char *text = screen_provider_settings_edit_text(s->edits[f]);
            int n = screen_provider_settings_line_count(text);
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
    screen_provider_settings_load_status(s, false);
    int n = (int)(sizeof screen_provider_settings_LOGIN_WAITS_MS / sizeof *screen_provider_settings_LOGIN_WAITS_MS);
    if (++s->login_wait < n && s->device_code) SetTimer(pane_hwnd(base->pane), TIMER_LOGIN, (UINT)(screen_provider_settings_LOGIN_WAITS_MS[s->login_wait] - screen_provider_settings_LOGIN_WAITS_MS[s->login_wait - 1]), NULL);
}

static bool form_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_provider_settings_form_save((FormScreen *)base); return true; }
    return false;
}

static void form_refresh(Screen *base) { screen_provider_settings_load_status((FormScreen *)base, false); }

static void form_visible(Screen *base, bool shown) {
    FormScreen *s = (FormScreen *)base;
    s->shown = shown;
    if (shown) {
        screen_provider_settings_load_binaries(s);
        screen_provider_settings_form_ensure_controls(s);
        if (s->id && !s->status && !s->req_status) screen_provider_settings_load_status(s, false);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool form_can_leave(Screen *base) {
    FormScreen *s = (FormScreen *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this provider") : xstrdup("The new provider has not been saved.");
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) { form_release(s); return true; }
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable form_vt = {
    .destroy = form_destroy, .layout = screen_provider_settings_form_layout, .header = screen_provider_settings_form_header, .action = form_action, .place = screen_provider_settings_form_place,
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
    if (!s->id) { s->focus_first = F_LABEL; screen_provider_settings_g_tab = T_PROVIDER; }
    screen_provider_settings_form_fill(s);
    return &s->base;
}
