#include "screen_mcp_settings_internal.h"

static char *form_id(double id);
static void open_sign_in(const char *url);
static void status_update(McpForm *s, const Json *row);
static void adopt_id(McpForm *s, double id);
static void read_done(void *owner, Request *req);
static void load(McpForm *s);
static void write_done(void *owner, Request *req);
static void write_call(McpForm *s, const char *operation, Json *args, int tag);
static void toggle_repo(McpForm *s, const char *repo);
static void form_action(Screen *base, int action, intptr_t arg, POINT pt);
static void form_command(Screen *base, int id, int code, HWND control);
static void departed_write_done(void *owner, Request *req);
static void detach_write(McpForm *s);
static void form_visible(Screen *base, bool shown);
static void form_timer(Screen *base, UINT id);
static void form_refresh(Screen *base);
static bool form_leave(Screen *base);
static void form_destroy(Screen *base);

// MCP registry only; core mounts the tools and runs commands on its own machine.

bool mcp_settings_supported(const char *operation) {
    return g_store.has_device && str_eq(g_store.device.permission, "admin") && store_supports(operation);
}

static char *form_id(double id) { return id ? xstrfmt("settings-mcp:%.0f", id) : xstrdup("settings-mcp:new"); }

bool screen_mcp_settings_account_current(McpForm *s) { return s->account && s->account == g_store.client && mcp_settings_supported("settings_mcp_servers"); }

static void open_sign_in(const char *url) {
    if (!mcp_secure_url(url)) return;
    wchar_t *w = utf8_to_wide(url);
    ShellExecuteW(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL); free(w);
}

bool screen_mcp_settings_busy(McpForm *s) { return s->write != NULL; }

bool screen_mcp_settings_can_save(McpForm *s) { return !screen_mcp_settings_busy(s) && (s->dirty || !s->id) && (!s->uncertain || (!s->id && s->create_retry)); }

static void status_update(McpForm *s, const Json *row) {
    char *previous = xstrdup(s->sign_in.url); mcp_sign_in_update(&s->sign_in, row);
    if (!str_eq(previous, s->sign_in.url) && s->edits[F_CALLBACK]) screen_mcp_settings_set_text(s->edits[F_CALLBACK], "");
    free(previous);
    static const char *const config[] = { "name", "label", "transport", "url", "command", "args", "repos", "enabled", "oauthClientId", "oauthScope", "oauthClientName", "oauthRedirect", "headerNames", "envNames", "hasOAuthClientSecret" };
    bool different = false;
    for (size_t i = 0; i < sizeof config / sizeof *config; i++) if (!json_equal(json_get(s->row, config[i]), json_get(row, config[i]))) different = true;
    json_free(s->row); s->row = json_clone(row);
    if (different) { if (s->dirty) s->conflict = true; else screen_mcp_settings_fill(s); }
}

static void adopt_id(McpForm *s, double id) {
    s->id = id; free(s->base.id); s->base.id = form_id(id); pane_set_selected_id(app_sidebar_pane(), s->base.id);
}

static void read_done(void *owner, Request *req) {
    McpForm *s = owner;
    if (!s->shown || !screen_mcp_settings_account_current(s) || req->client != s->account) return;
    const Json *rows = json_get(req->result, "servers"), *row = NULL;
    bool creating = !s->id && s->uncertain && s->create_name;
    bool listed = req->ok && json_is_array(rows);
    if (listed) for (size_t i = 0; i < json_count(rows); i++) {
        const Json *candidate = json_at(rows, i);
        if (mcp_server_id(candidate) && (creating ? str_eq(json_str(json_get(candidate, "name")), s->create_name) : mcp_server_id(candidate) == s->id)) row = candidate;
    }
    if (row) {
        bool visible_changed = creating;
        static const char *const visible[] = { "label", "transport", "status", "enabled" };
        for (size_t i = 0; i < sizeof visible / sizeof *visible; i++) if (!json_equal(json_get(s->row, visible[i]), json_get(row, visible[i]))) visible_changed = true;
        if (creating) adopt_id(s, mcp_server_id(row));
        status_update(s, row);
        if (visible_changed) settings_mcp_changed();
        if (s->uncertain || s->read_error) set_string(&s->error, creating ? "The created server was found. Your draft is preserved; review it before saving any further changes." : NULL);
        s->read_error = false;
        s->uncertain = false; s->create_retry = false;
    } else if (creating && listed) {
        s->read_error = false;
        s->create_retry = true;
        set_string(&s->error, "The submitted name is not listed yet; the create may still complete. Save retries with the original name. Your draft name is preserved for an update after the server is found.");
    }
    else {
        mcp_sign_in_failed(&s->sign_in);
        if (s->edits[F_CALLBACK]) screen_mcp_settings_set_text(s->edits[F_CALLBACK], "");
        s->read_error = !listed;
        if (listed) { s->uncertain = true; settings_mcp_changed(); set_string(&s->error, "This MCP server is no longer listed. Reopen it from Settings."); }
        else { char *e = xstrfmt("MCP status could not be refreshed (HTTP %d). Refresh or reconnect before finishing sign-in.", req->error.status); set_string(&s->error, e); free(e); }
    }
    poller_finished(&s->poll, !listed, req->error.retry_after); screen_mcp_settings_repaint(s);
}

static void load(McpForm *s) {
    if (!s->shown || !screen_mcp_settings_account_current(s) || s->read || screen_mcp_settings_busy(s)) return;
    if (!s->id && !(s->uncertain && s->create_name)) return;
    store_call("settings_mcp_servers", json_object(), 0, s, read_done, 0, &s->read);
}

static void write_done(void *owner, Request *req) {
    McpForm *s = owner;
    if (!s->shown || !screen_mcp_settings_account_current(s) || req->client != s->account) return;
    if (req->tag == ACT_FINISH) { screen_mcp_settings_set_text(s->edits[F_CALLBACK], ""); if (!req->ok) mcp_sign_in_failed(&s->sign_in); }
    const Json *row = json_get(req->result, "server");
    if (!req->ok || (req->tag != ACT_DELETE && (!mcp_server_id(row) || (s->id && mcp_server_id(row) != s->id)))) {
        char *e = req->ok ? xstrdup("Unexpected MCP response; refresh before trying again.") : xstrfmt("MCP action failed (HTTP %d). Refresh status; sign-in callbacks are single-use and must not be resubmitted automatically.", req->error.status);
        screen_mcp_settings_error_text(s, e); free(e);
        // Create may commit before a follow-up connection/status save returns an HTTP error.
        s->uncertain = s->uncertain || req->ok || request_outcome_unknown(req) || (req->tag == ACT_SAVE && !s->id);
        // Reads recover an unknown outcome; honor rate limits/unavailability before that read.
        if (req->error.status != 429 && req->error.status != 503) load(s);
        poller_finished(&s->poll, true, req->error.retry_after); return;
    }
    if (req->tag == ACT_DELETE) { s->dirty = false; settings_mcp_changed(); app_clear_detail(); return; }
    char *draft_name = req->tag == ACT_SAVE && !s->id ? screen_mcp_settings_edit_text(s->edits[F_NAME]) : NULL;
    adopt_id(s, mcp_server_id(row)); status_update(s, row); s->uncertain = false; s->create_retry = false; s->read_error = false; set_string(&s->error, NULL);
    if (req->tag == ACT_SAVE) {
        screen_mcp_settings_fill(s);
        if (draft_name && !str_eq(draft_name, json_str(json_get(row, "name")))) { screen_mcp_settings_set_text(s->edits[F_NAME], draft_name); screen_mcp_settings_changed(s); }
    }
    free(draft_name);
    settings_mcp_changed(); screen_mcp_settings_repaint(s);
    poller_finished(&s->poll, false, -1);
    if ((req->tag == ACT_SAVE || req->tag == ACT_CONNECT || req->tag == ACT_SIGN_IN) && s->sign_in.url && !s->sign_in.blocked) open_sign_in(s->sign_in.url);
}

static void write_call(McpForm *s, const char *operation, Json *args, int tag) {
    if (!s->shown || !screen_mcp_settings_account_current(s) || !mcp_settings_supported(operation)) { json_free(args); return; }
    if (tag == ACT_SAVE && !s->id) {
        if (s->uncertain && s->create_name) json_set_str(args, "name", s->create_name);
        else set_string(&s->create_name, json_str(json_get(args, "name")));
        s->create_retry = false;
    }
    request_cancel(&s->read); if (s->id) json_set_num(args, "id", s->id);
    store_call(operation, args, 60000, s, write_done, tag, &s->write); screen_mcp_settings_repaint(s);
}

void screen_mcp_settings_form_save(McpForm *s) {
    if (!screen_mcp_settings_account_current(s) || !s->shown || !screen_mcp_settings_can_save(s) || !mcp_settings_supported(s->id ? "update_mcp_server" : "create_mcp_server")) return;
    char *why = NULL; Json *body = screen_mcp_settings_body_now(s, &why);
    if (!body) { screen_mcp_settings_error_text(s, why); free(why); return; }
    if (s->conflict && !app_confirm("Replace newer MCP configuration?", "This server changed on core while you were editing. Saving replaces its current configuration with your draft. Cancel to keep editing or reload the current fields.", "Replace", true)) { json_free(body); return; }
    write_call(s, s->id ? "update_mcp_server" : "create_mcp_server", body, ACT_SAVE);
}

static void toggle_repo(McpForm *s, const char *repo) {
    Json *next = json_array(); bool had = screen_mcp_settings_selected(s, repo);
    for (size_t i = 0; i < json_count(s->repos); i++) if (!str_eq(repo, json_str(json_at(s->repos, i)))) json_array_push(next, json_clone(json_at(s->repos, i)));
    if (!had) json_array_push(next, json_string(repo));
    json_free(s->repos); s->repos = next; screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s);
}

static void form_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt; McpForm *s = (McpForm *)base;
    if (!s->shown || !screen_mcp_settings_account_current(s)) return;
    if (action == ACT_TAB && arg >= 0 && arg < 3) { s->tab = (int)arg; screen_mcp_settings_repaint(s); pane_scroll_to_top(base->pane); return; }
    if (action == ACT_FOCUS && arg >= 0 && arg < F_COUNT) { SetFocus(s->edits[arg]); return; }
    if (screen_mcp_settings_busy(s)) return;
    switch (action) {
    case ACT_SAVE: screen_mcp_settings_form_save(s); break;
    case ACT_RELOAD:
        if (s->conflict && app_confirm("Discard unsaved MCP changes?", "Reload the latest server fields and discard this draft, including replacement secrets.", "Reload", true)) { screen_mcp_settings_fill(s); screen_mcp_settings_repaint(s); }
        break;
    case ACT_DELETE:
        if (s->id && mcp_settings_supported("delete_mcp_server") && app_confirm("Delete MCP server?", "Assigned sessions will lose these tools on their next turn.", "Delete", true)) write_call(s, "delete_mcp_server", json_object(), ACT_DELETE);
        break;
    case ACT_TRANSPORT: s->stdio = !s->stdio; screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s); break;
    case ACT_ENABLED: s->enabled = !s->enabled; screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s); break;
    case ACT_ALL: s->all = !s->all; screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s); break;
    case ACT_REDIRECT: s->loopback = !s->loopback; screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s); break;
    case ACT_REPO: {
        const char *repo = arg < 0 ? json_str(json_at(s->repos, (size_t)(-arg - 1))) : json_str(json_get(json_at(settings_project_rows(), (size_t)arg), "repo"));
        if (repo) toggle_repo(s, repo);
        break;
    }
    case ACT_SECRET:
        if (arg >= 0 && arg < 3) { s->secrets[arg] = (McpSecretMode)((s->secrets[arg] + 1) % 3); screen_mcp_settings_changed(s); screen_mcp_settings_repaint(s); } break;
    case ACT_CONNECT: case ACT_SIGN_IN:
        if (s->id && !s->dirty && !s->uncertain && mcp_settings_supported("connect_mcp_server")) {
            Json *body = json_object(); json_set_bool(body, "signIn", action == ACT_SIGN_IN);
            write_call(s, "connect_mcp_server", body, action);
        } break;
    case ACT_BROWSER: if (!s->dirty && s->sign_in.url && !s->sign_in.blocked) open_sign_in(s->sign_in.url); break;
    case ACT_FINISH:
        if (s->id && !s->dirty && !s->uncertain && mcp_sign_in_can_finish(&s->sign_in) && mcp_settings_supported("finish_mcp_sign_in")) {
            char *url = screen_mcp_settings_edit_text(s->edits[F_CALLBACK]), *trim = str_trim(url); free(url);
            if (!mcp_callback_url(trim)) { free(trim); screen_mcp_settings_error_text(s, "Paste the complete callback URL with state and code (or provider error)."); break; }
            Json *body = json_object(); json_set_str(body, "url", trim); free(trim); write_call(s, "finish_mcp_sign_in", body, ACT_FINISH);
        } break;
    }
}

static void form_command(Screen *base, int id, int code, HWND control) {
    (void)control; McpForm *s = (McpForm *)base; int f = id - ID_FIELD;
    if (f >= 0 && f < F_CALLBACK && code == EN_CHANGE && !screen_mcp_settings_busy(s)) screen_mcp_settings_changed(s);
}

static void departed_write_done(void *owner, Request *req) {
    (void)owner;
    if (req->client && req->client == g_store.client && mcp_settings_supported("settings_mcp_servers")) settings_mcp_changed();
}

static void detach_write(McpForm *s) {
    if (!s->write) return;
    // The worker continues on core. Observe completion without retaining the form or its controls.
    s->write->owner = NULL; s->write->slot = NULL; s->write->done = departed_write_done;
    s->write = NULL; s->uncertain = true; s->create_retry = false; s->read_error = false;
    set_string(&s->error, "An action is still completing on core. Refresh status before repeating it.");
}

static void form_visible(Screen *base, bool shown) {
    McpForm *s = (McpForm *)base; s->shown = shown;
    if (shown && screen_mcp_settings_account_current(s)) { screen_mcp_settings_ensure_controls(s); poller_start(&s->poll, base->pane, TIMER_STATUS, 5000); load(s); }
    else {
        poller_stop(&s->poll); request_cancel(&s->read);
        detach_write(s); mcp_sign_in_clear(&s->sign_in);
        for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) { if (f == F_CALLBACK) screen_mcp_settings_set_text(s->edits[f], ""); ShowWindow(s->edits[f], SW_HIDE); }
    }
}

static void form_timer(Screen *base, UINT id) {
    McpForm *s = (McpForm *)base;
    if (!screen_mcp_settings_account_current(s)) { form_visible(base, false); screen_mcp_settings_repaint(s); return; }
    if (poller_fired(&s->poll, id)) { if (screen_mcp_settings_busy(s) || (!s->id && !s->uncertain)) poller_finished(&s->poll, false, -1); else load(s); }
}

static void form_refresh(Screen *base) { McpForm *s = (McpForm *)base; request_cancel(&s->read); load(s); }

static bool form_leave(Screen *base) { McpForm *s = (McpForm *)base; return !s->dirty || app_confirm("Discard unsaved MCP changes?", "The edited server fields have not been saved.", "Discard", true); }

static void form_destroy(Screen *base) {
    McpForm *s = (McpForm *)base; poller_stop(&s->poll); request_cancel(&s->read); detach_write(s);
    for (int f = 0; f < F_COUNT; f++) if (s->edits[f]) DestroyWindow(s->edits[f]);
    json_free(s->row); json_free(s->repos); free(s->error); free(s->create_name); mcp_sign_in_clear(&s->sign_in); api_client_release(s->account); screen_release(base);
}

static const ScreenVTable VT = { .destroy = form_destroy, .layout = screen_mcp_settings_form_layout, .header = screen_mcp_settings_form_header, .action = form_action, .place = screen_mcp_settings_form_place, .visible = form_visible, .command = form_command, .timer = form_timer, .refresh = form_refresh, .can_leave = form_leave };

Screen *mcp_settings_screen_new(const Json *row, const Json *defaults) {
    McpForm *s = xcalloc(1, sizeof *s); s->base.vt = &VT; s->account = api_client_retain(g_store.client);
    s->row = json_is_object(row) ? json_clone(row) : json_is_object(defaults) ? json_clone(defaults) : json_object();
    s->id = mcp_server_id(s->row); s->base.id = form_id(s->id); screen_mcp_settings_fill(s);
    return &s->base;
}
