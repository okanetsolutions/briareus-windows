#include "screen_settings_github_internal.h"

static void github_show_error(GithubForm *s, char *text);
static void github_save_done(void *owner, Request *req);
static void github_write_error(GithubForm *s, Request *req);
static void github_delete_done(void *owner, Request *req);
static void github_delete(GithubForm *s);
static void github_destroy(Screen *base);
static void github_release(GithubForm *s);
static void github_action(Screen *base, int action, intptr_t arg, POINT pt);
static void github_command(Screen *base, int id, int code, HWND control);
static bool github_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void github_visible(Screen *base, bool shown);
static bool github_can_leave(Screen *base);

bool screen_settings_github_busy(const GithubForm *s) { return s->req_save || s->req_delete; }

/// Whether the form holds a change not saved yet, for the dot after the tab's title.
bool screen_settings_github_form_changed(GithubForm *s) {
    if (!screen_settings_repos_same(s->repos, json_get(s->row, "repos"))) return true;
    for (int f = 0; f < GH_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *now = screen_settings_edit_text(s->edits[f]), *saved = screen_settings_github_field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}

void screen_settings_github_changed(GithubForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = screen_settings_github_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Saving, deleting

static void github_show_error(GithubForm *s, char *text) {
    set_string(&s->error, text); free(text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}

static void github_write_error(GithubForm *s, Request *req) {
    char *text = NULL;
    if (req->ok) text = xstrdup("The server returned an unexpected response. It may still have been saved; refresh before trying again.");
    // These account verification failures happen before create/update saves anything.
    else if (req->error.kind == API_HTTP && (req->error.status == 502 || req->error.status == 503) && !str_eq(req->operation, "delete_github_account"))
        request_error_into(&text, req);
    else request_write_error_into(&text, req);
    github_show_error(s, text);
}

static void github_save_done(void *owner, Request *req) {
    GithubForm *s = owner;
    screen_settings_github_controls(s);
    if (!screen_settings_github_current(s) || req->client != s->client) return;
    const Json *row = req->ok ? json_get(req->result, "account") : NULL;
    if (!json_is_object(row) || !screen_settings_ssh_row_id(row)) { github_write_error(s, req); return; }
    // Only a successful reply replaces the draft and clears the write-only token.
    json_free(s->row); s->row = json_clone(row);
    json_object_remove(s->row, "token");
    set_string(&s->error, NULL);
    s->id = screen_settings_ssh_row_id(row);
    free(s->base.id); s->base.id = screen_settings_github_form_id(s->id);
    settings_select_id(s->base.id);
    screen_settings_github_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    screen_settings_github_changed_list();
}

void screen_settings_github_save(GithubForm *s) {
    if (s->modal || !screen_settings_github_current(s)) return;
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_github_account" : "create_github_account")) return;
    char *why = NULL;
    Json *body = screen_settings_github_body(s, &why);
    if (!body) { github_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_github_account" : "create_github_account", body, 0, s, github_save_done, 0, &s->req_save);
    screen_settings_github_controls(s); pane_header_changed(s->base.pane);
}

static void github_delete_done(void *owner, Request *req) {
    GithubForm *s = owner;
    screen_settings_github_controls(s);
    if (!screen_settings_github_current(s) || req->client != s->client) return;
    if (!req->ok || !json_bool_is(json_get(req->result, "ok"), true)) { github_write_error(s, req); return; }
    s->dirty = false;
    Pane *pane = s->base.pane;
    screen_settings_github_changed_list();
    if (pane_top(pane) != &s->base) return;
    if (pane_root(pane) != &s->base) pane_pop(pane);
    else if (pane == app_detail_pane()) app_clear_detail();
    else pane_set_root(pane, NULL);
}

static void github_delete(GithubForm *s) {
    if (!screen_settings_github_current(s) || !store_supports("delete_github_account")) return;
    if (!s->id || s->req_save || s->req_delete || s->modal) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this GitHub account");
    s->modal = true;
    bool ok = app_confirm(title, "Assigned projects fall back to GITHUB_TOKEN, or fail until reassigned if no server default token is available.", "Delete", true);
    s->modal = false;
    free(title);
    if (s->retired) { github_release(s); return; }
    if (!ok || !screen_settings_github_current(s)) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_github_account", args, 0, s, github_delete_done, 0, &s->req_delete);
    screen_settings_github_controls(s); pane_header_changed(s->base.pane);
}

// MARK: The screen

static void github_release(GithubForm *s) {
    for (int f = 0; f < GH_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->repos); json_free(s->row); free(s->error);
    api_client_release(s->client); free(s->device);
    screen_release(&s->base);
}
static void github_destroy(Screen *base) {
    GithubForm *s = (GithubForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete);
    if (s->modal) { s->retired = true; return; }
    github_release(s);
}

static void github_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    GithubForm *s = (GithubForm *)base;
    switch (action) {
    case ACT_GITHUB_SAVE: screen_settings_github_save(s); break;
    case ACT_GITHUB_DELETE: github_delete(s); break;
    case ACT_GITHUB_REPO: if (arg >= 0) screen_settings_github_toggle_repo(s, (size_t)arg); break;
    case ACT_GITHUB_FOCUS: if (arg >= 0 && arg < GH_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    }
}

static void github_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    GithubForm *s = (GithubForm *)base;
    int f = id - ID_GITHUB_FIELD;
    if (f < 0 || f >= GH_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: screen_settings_github_changed(s); break;
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

static bool github_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_settings_github_save((GithubForm *)base); return true; }
    return false;
}

static void github_visible(Screen *base, bool shown) {
    GithubForm *s = (GithubForm *)base;
    s->shown = shown;
    if (shown) {
        screen_settings_github_ensure_controls(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < GH_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool github_can_leave(Screen *base) {
    GithubForm *s = (GithubForm *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this GitHub account") : xstrdup("The new GitHub account has not been saved.");
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) { github_release(s); return true; }
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable github_vt = {
    .destroy = github_destroy, .layout = screen_settings_github_layout, .header = screen_settings_github_header, .action = github_action, .place = screen_settings_github_place,
    .visible = github_visible, .command = github_command, .key = github_key, .can_leave = github_can_leave,
};

Screen *screen_settings_github_settings_screen_new(const Json *row, const Json *defaults) {
    GithubForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &github_vt;
    s->client = api_client_retain(g_store.client); s->device = xstrdup(g_store.device.id);
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    json_object_remove(s->row, "token");
    s->id = screen_settings_ssh_row_id(s->row);
    s->base.id = screen_settings_github_form_id(s->id);
    // The token is required only for a new account.
    if (!s->id) s->focus_first = GH_TOKEN;
    screen_settings_github_fill(s);
    return &s->base;
}
