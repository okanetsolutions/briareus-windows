#include "screen_settings_internal.h"

static void ssh_show_error(SshForm *s, char *text);
static void ssh_save_done(void *owner, Request *req);
static void ssh_delete_done(void *owner, Request *req);
static void ssh_delete(SshForm *s);
static void ssh_clone(SshForm *s);
static void ssh_destroy(Screen *base);
static void ssh_action(Screen *base, int action, intptr_t arg, POINT pt);
static void ssh_command(Screen *base, int id, int code, HWND control);
static bool ssh_key(Screen *base, WPARAM vk, bool ctrl, bool shift);
static void ssh_visible(Screen *base, bool shown);
static bool ssh_can_leave(Screen *base);

// MARK: Saving, cloning, deleting

static void ssh_show_error(SshForm *s, char *text) {
    set_string(&s->error, text); free(text);
    screen_settings_g_ssh_tab = s->error_tab;
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane); pane_scroll_to_top(s->base.pane);
}

static void ssh_save_done(void *owner, Request *req) {
    SshForm *s = owner;
    if (req->client != s->account || !screen_settings_ssh_current(s)) return;
    const Json *row = req->ok ? json_get(req->result, "server") : NULL;
    double saved_id = screen_settings_ssh_row_id(row);
    if (!json_is_object(row) || saved_id <= 0 || (s->id && saved_id != s->id)) {
        char *text = xstrdup(request_outcome_unknown(req) || req->ok ? "The save outcome is unknown. Your draft is retained; check the server before saving again." : "The server refused the save. Your draft is retained.");
        s->error_tab = screen_settings_g_ssh_tab;
        ssh_show_error(s, text);
        return;
    }
    // The server's word on what was saved: an empty label is now user@host:port, and a new server has an id.
    // The username just sent is now the stored one; a server without a login has none.
    const Json *sent = json_get(req->args, "dbUsername");
    if (!json_bool_is(json_get(row, "hasDbCredentials"), true)) set_string(&s->db_user, "");
    else if (json_str(sent)) set_string(&s->db_user, json_str(sent));
    else if (!s->db_user && !s->id) set_string(&s->db_user, "");
    bool confirmed = ssh_secrets_saved(&s->secrets, row, req->args, s->sent_secrets);
    json_free(s->row); s->row = json_clone(row);
    for (int f = 0; f < SSH_SECRET_COUNT; f++) json_object_remove(s->row, SSH_SECRET_KEYS[f]);
    set_string(&s->error, NULL);
    s->id = screen_settings_ssh_row_id(row);
    free(s->base.id); s->base.id = screen_settings_ssh_form_id(s->id);
    settings_select_id(s->base.id);
    if (s->revision == s->sent_revision) screen_settings_ssh_fill(s);
    else screen_settings_ssh_secret_controls(s);
    s->dirty = screen_settings_ssh_tab_changed(s, ST_SERVER) || screen_settings_ssh_tab_changed(s, ST_DATABASE);
    if (!confirmed) ssh_show_error(s, xstrdup("The response did not confirm the password change. Your draft is retained; check the server before saving again."));
    // A server that answers without the login's fields did not keep it: say so rather than show an empty form as saved.
    bool sent_login = json_str(json_get(req->args, "dbUsername")) || json_str(json_get(req->args, "dbPassword"));
    const Json *has = json_get(row, "hasDbCredentials");
    if (sent_login && !json_bool_is(has, true) && !json_bool_is(has, false)) {
        s->error_tab = ST_DATABASE;
        ssh_show_error(s, xstrdup("The server saved the SSH server but not its database login: it does not store database logins yet. Update the Briareus server."));
    }
    pane_relayout(s->base.pane); pane_header_changed(s->base.pane);
    screen_settings_settings_ssh_changed();
}

void screen_settings_ssh_save(SshForm *s) {
    if (!screen_settings_ssh_current(s)) return;
    if (s->req_save || s->req_delete || !store_supports(s->id ? "update_ssh_server" : "create_ssh_server")) return;
    char *why = NULL;
    Json *body = screen_settings_ssh_body(s, &why);
    if (!body) { ssh_show_error(s, why); return; }
    if (s->id) json_set_num(body, "id", s->id);
    s->sent_revision = s->revision;
    for (int f = 0; f < SSH_SECRET_COUNT; f++) s->sent_secrets[f] = s->secrets.fields[f].revision;
    store_call(s->id ? "update_ssh_server" : "create_ssh_server", body, 0, s, ssh_save_done, 0, &s->req_save);
    pane_header_changed(s->base.pane);
}

static void ssh_delete_done(void *owner, Request *req) {
    SshForm *s = owner;
    if (!req->ok) { s->error_tab = screen_settings_g_ssh_tab; ssh_show_error(s, request_error_text(req)); return; }
    s->dirty = false;
    app_clear_detail();
    screen_settings_settings_ssh_changed();
}

static void ssh_delete(SshForm *s) {
    if (!s->id || s->req_save || s->req_delete) return;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *title = xstrfmt("Delete %s?", name ? name : "this SSH server");
    bool ok = app_confirm(title, "Sessions lose access through the SSH tool and pending approvals are cancelled.", "Delete", true);
    free(title);
    if (!ok) return;
    Json *args = json_object(); json_set_num(args, "id", s->id);
    store_call("delete_ssh_server", args, 0, s, ssh_delete_done, 0, &s->req_delete);
    pane_header_changed(s->base.pane);
}

static void ssh_clone(SshForm *s) {
    char *why = NULL;
    Json *copy = screen_settings_ssh_body(s, &why);
    if (!copy) { ssh_show_error(s, why); return; }
    // The copy carries what the form holds now, saved or not, without the label that named this one. The database
    // username goes with it; a stored password is never read back, so only one typed here does.
    json_set_str(copy, "label", "");
    if (s->edits[S_DBUSER]) { char *text = screen_settings_edit_text(s->edits[S_DBUSER]), *t = str_trim(text); json_set_str(copy, "dbUsername", t); free(text); free(t); }
    s->dirty = false;
    Screen *clone = screen_settings_ssh_settings_screen_new(copy, NULL);
    SshForm *form = (SshForm *)clone;
    ssh_secrets_free(&form->secrets); ssh_secrets_clone(&form->secrets, &s->secrets);
    for (int f = 0; f < SSH_SECRET_COUNT; f++) json_object_remove(form->row, SSH_SECRET_KEYS[f]);
    form->dirty = ssh_secrets_changed(&form->secrets);
    ((SshForm *)clone)->focus_first = S_LABEL;
    screen_settings_g_ssh_tab = ST_SERVER;
    ssh_secrets_scrub(copy); json_free(copy);
    app_show_detail(clone);
}

// MARK: The screen

static void ssh_release(SshForm *s) {
    s->filling = true;
    for (int f = 0; f < S_COUNT; f++) if (s->edits[f]) { screen_settings_set_edit_text(s->edits[f], ""); DestroyWindow(s->edits[f]); }
    for (int p = 0; p < SP_COUNT; p++) free(s->picks[p]);
    ssh_secrets_free(&s->secrets); api_client_release(s->account); free(s->device);
    json_free(s->row); free(s->error); free(s->db_user); free(s->login_error);
    screen_release(&s->base);
}
static void ssh_destroy(Screen *base) {
    SshForm *s = (SshForm *)base;
    request_cancel(&s->req_save); request_cancel(&s->req_delete); request_cancel(&s->req_login);
    if (s->modal) { s->retired = true; return; }
    ssh_release(s);
}

static void ssh_action(Screen *base, int action, intptr_t arg, POINT pt) {
    SshForm *s = (SshForm *)base;
    if (s->retired || !screen_settings_ssh_current(s)) return;
    switch (action) {
    case ACT_SSH_SECRET:
        ssh_secrets_toggle(&s->secrets, (int)arg);
        screen_settings_ssh_secret_controls(s); screen_settings_ssh_changed(s); pane_relayout(base->pane); break;
    case ACT_SSH_SAVE: screen_settings_ssh_save(s); break;
    case ACT_SSH_CLONE: ssh_clone(s); break;
    case ACT_SSH_DELETE: ssh_delete(s); break;
    case ACT_SSH_TOGGLE: s->enabled = !s->enabled; screen_settings_ssh_changed(s); pane_relayout(base->pane); break;
    case ACT_SSH_PICK: if (arg >= 0 && arg < SP_COUNT) screen_settings_ssh_pick(s, (int)arg, pt); break;
    case ACT_SSH_FOCUS: if (arg >= 0 && arg < S_COUNT && s->edits[arg]) SetFocus(s->edits[arg]); break;
    case ACT_SSH_TAB:
        if (arg < 0 || arg >= ST_COUNT) break;
        if (s->focused >= 0) SetFocus(pane_hwnd(base->pane));
        screen_settings_g_ssh_tab = (int)arg;
        pane_relayout(base->pane); pane_scroll_to_top(base->pane);
        break;
    }
}

static void ssh_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    SshForm *s = (SshForm *)base;
    int f = id - ID_SSH_FIELD;
    if (f < 0 || f >= S_COUNT || !s->edits[f]) return;
    switch (code) {
    case EN_CHANGE:
        if (s->filling) break;
        if (f == S_PASSWORD || f == S_SUDO_PASSWORD) {
            char *text = screen_settings_ssh_secret_text(s->edits[f]);
            ssh_secrets_set(&s->secrets, f - S_PASSWORD, text, strlen(text));
            ssh_secret_free(text, strlen(text));
            screen_settings_ssh_secret_cues(s);
        }
        screen_settings_ssh_changed(s); break;
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

static bool ssh_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_settings_ssh_save((SshForm *)base); return true; }
    return false;
}

static void ssh_visible(Screen *base, bool shown) {
    SshForm *s = (SshForm *)base;
    s->shown = shown;
    if (shown) {
        screen_settings_ssh_ensure_controls(s);
        screen_settings_ssh_load_login(s);
        if (s->focus_first >= 0) { int f = s->focus_first; s->focus_first = -1; SetFocus(s->edits[f]); }
    } else for (int f = 0; f < S_COUNT; f++) if (s->edits[f]) ShowWindow(s->edits[f], SW_HIDE);
}

static bool ssh_can_leave(Screen *base) {
    SshForm *s = (SshForm *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const char *name = json_str_nonempty(json_get(s->row, "label"));
    char *message = s->id ? xstrfmt("The changes to %s have not been saved.", name ? name : "this SSH server") : xstrdup("The new SSH server has not been saved.");
    s->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) { ssh_release(s); return true; }
    if (leave) {
        ssh_secrets_free(&s->secrets); ssh_secrets_init(&s->secrets, s->row, NULL, false);
        screen_settings_ssh_secret_controls(s); s->dirty = false;
    }
    return leave;
}

static const ScreenVTable ssh_vt = {
    .destroy = ssh_destroy, .layout = screen_settings_ssh_layout, .header = screen_settings_ssh_header, .action = ssh_action, .place = screen_settings_ssh_place,
    .visible = ssh_visible, .command = ssh_command, .key = ssh_key, .can_leave = ssh_can_leave,
};

Screen *screen_settings_ssh_settings_screen_new(const Json *row, const Json *defaults) {
    SshForm *s = xcalloc(1, sizeof *s);
    s->base.vt = &ssh_vt;
    s->account = api_client_retain(g_store.client); s->device = xstrdup(g_store.device.id);
    s->focused = -1; s->focus_first = -1;
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = screen_settings_ssh_row_id(s->row);
    s->base.id = screen_settings_ssh_form_id(s->id);
    SettingsScreen *settings = screen_settings_g_settings;
    bool current = settings && settings->ssh_loaded && settings->ssh_account == g_store.client && str_eq(settings->ssh_device, g_store.device.id);
    const Json *rows = current ? screen_settings_ssh_rows(settings) : NULL;
    ssh_secrets_init(&s->secrets, s->row, rows, !s->id);
    for (int f = 0; f < SSH_SECRET_COUNT; f++) json_object_remove(s->row, SSH_SECRET_KEYS[f]);
    if (!s->id) {
        // A new server starts on the first project, as the web's form does, and where its label is typed.
        if (!json_str_nonempty(json_get(s->row, "repo")) && screen_settings_g_settings) {
            const char *first = json_str_nonempty(json_get(json_at(screen_settings_settings_rows(screen_settings_g_settings), 0), "repo"));
            if (first) json_set_str(s->row, "repo", first);
        }
        if (!json_get(s->row, "port")) json_set_num(s->row, "port", 22);
        if (!row) { s->focus_first = S_LABEL; screen_settings_g_ssh_tab = ST_SERVER; }
    }
    screen_settings_ssh_fill(s);
    return &s->base;
}
