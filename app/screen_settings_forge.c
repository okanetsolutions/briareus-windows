#include "screen_settings_internal.h"

static void forge_show_error(ForgeForm *s, char *text);
static void forge_save_done(void *owner, Request *req);
static void forge_delete_done(void *owner, Request *req);
static void forge_delete(ForgeForm *s);
static void forge_destroy(Screen *base);
static void forge_action(Screen *base, int action, intptr_t arg, POINT pt);
static void forge_command(Screen *base, int id, int code, HWND control);
static bool forge_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void forge_visible(Screen *base, bool shown);
static bool forge_can_leave(Screen *base);

/// Whether the form holds a change not saved yet, for the dot after the tab's title.
bool screen_settings_forge_form_changed(ForgeForm *s) {
    if (!screen_settings_repos_same(s->repos, json_get(s->row, "repos"))) return true;
    for (int f = 0; f < G_COUNT; f++) {
        if (!s->edits[f]) continue;
        char *now = screen_settings_edit_text(s->edits[f]), *saved = screen_settings_forge_field_text(s->row, f);
        bool differs = !str_eq(now, saved);
        free(now); free(saved);
        if (differs) return true;
    }
    return false;
}

void screen_settings_forge_changed(ForgeForm *s) {
    if (s->filling) return;
    if (!s->dirty) { s->dirty = true; pane_header_changed(s->base.pane); }
    bool changed = screen_settings_forge_form_changed(s);
    if (changed != s->tab_dot) { s->tab_dot = changed; pane_relayout(s->base.pane); }
}

// MARK: Saving, deleting

static void forge_show_error(ForgeForm *s, char *text) {
    set_string(&s->error, text); free(text);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}

static void forge_save_done(void *owner, Request *req) {
    ForgeForm *s = owner;
    const Json *row = req->ok ? json_get(req->result, "account") : NULL;
    if (!json_is_object(row)) {
        char *text = request_error_or_unexpected(req);
        forge_show_error(s, text);
        return;
    }
    // The server's word on what was saved: an empty label is now the organization, and a new account has an id.
    json_free(s->row); s->row = json_clone(row);
    set_string(&s->error, NULL);
    s->id = screen_settings_ssh_row_id(row);
    free(s->base.id); s->base.id = screen_settings_forge_form_id(s->id);
    settings_select_id(s->base.id);
    screen_settings_forge_fill(s);
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    screen_settings_settings_forge_changed();
}

void screen_settings_forge_save(ForgeForm *s) {
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_forge_account" : "create_forge_account")) return;
    char *why = NULL;
    Json *body = screen_settings_forge_body(s, &why);
    if (!body) { forge_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    store_call(s->id ? "update_forge_account" : "create_forge_account", body, 0, s, forge_save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}

static void forge_delete_done(void *owner, Request *req) {
    ForgeForm *s = owner;
    if (!req->ok) { forge_show_error(s, request_error_text(req)); return; }
    s->dirty = false;
    app_clear_detail();
    screen_settings_settings_forge_changed();
}

static void forge_delete(ForgeForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this Forge account");
    bool ok = app_confirm(title, "Its stored token is removed and clients can no longer reach the organization's Forge servers and sites through it.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_forge_account", args, 0, s, forge_delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}

// MARK: The screen

static void forge_release(ForgeForm *s) {
    for (int f = 0; f < G_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->repos); json_free(s->row); free(s->error);
    screen_release(&s->base);
}
static void forge_destroy(Screen *base) {
    ForgeForm *s = (ForgeForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete);
    if (s->modal) { s->retired = true; return; }
    forge_release(s);
}

static void forge_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ForgeForm *s = (ForgeForm *)base;
    switch (action) {
    case ACT_FORGE_SAVE: screen_settings_forge_save(s); break;
    case ACT_FORGE_DELETE: forge_delete(s); break;
    case ACT_FORGE_REPO: if (arg >= 0) screen_settings_forge_toggle_repo(s, (size_t)arg); break;
    case ACT_FORGE_FOCUS: if (arg >= 0 && arg < G_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    }
}

static void forge_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    ForgeForm *s = (ForgeForm *)base;
    int f = id - ID_FORGE_FIELD;
    if (f < 0 || f >= G_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE: screen_settings_forge_changed(s); break;
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

static bool forge_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_settings_forge_save((ForgeForm *)base); return true; }
    return false;
}

static void forge_visible(Screen *base, bool shown) {
    ForgeForm *s = (ForgeForm *)base;
    s->shown = shown;
    if (shown) {
        screen_settings_forge_ensure_controls(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < G_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool forge_can_leave(Screen *base) {
    ForgeForm *s = (ForgeForm *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this Forge account") : xstrdup("The new Forge account has not been saved.");
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) { forge_release(s); return true; }
    if (leave) s->dirty = false;
    return leave;
}

static const ScreenVTable forge_vt = {
    .destroy = forge_destroy, .layout = screen_settings_forge_layout, .header = screen_settings_forge_header, .action = forge_action, .place = screen_settings_forge_place,
    .visible = forge_visible, .command = forge_command, .key = forge_key, .can_leave = forge_can_leave,
};

Screen *screen_settings_forge_settings_screen_new(const Json *row, const Json *defaults) {
    ForgeForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &forge_vt;
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = screen_settings_ssh_row_id(s->row);
    s->base.id = screen_settings_forge_form_id(s->id);
    // A new account starts where its organization is typed, the one box it cannot do without.
    if (!s->id) s->focus_first = G_ORG;
    screen_settings_forge_fill(s);
    return &s->base;
}
