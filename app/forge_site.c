#include "forge_site_internal.h"

static const char *site_name(const ForgeSite *v);
static Json *site_args(const ForgeSite *v);
static void site_done(void *owner, Request *req);
static void site_load(ForgeSite *v);
static void text_done(void *owner, Request *req);
static void text_load(ForgeSite *v, int ed);
static void tab_load(ForgeSite *v);
static void save_done(void *owner, Request *req);

static const char *site_name(const ForgeSite *v) {
    const char *name = json_str_nonempty(json_get(v->site, "name"));
    return name ? name : "Forge site";
}

static Json *site_args(const ForgeSite *v) {
    Json *a = json_object();
    json_set_num(a, "account", v->account);
    json_set_num(a, "server", v->server_id);
    json_set_num(a, "site", v->site_id);
    return a;
}

int forge_site_margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

void forge_site_refresh_view(ForgeSite *v) {
    if (!v->host->pane) return;
    pane_relayout(v->host->pane);
    pane_header_changed(v->host->pane);
}

// MARK: - Loading

static void site_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    const Json *site = req->ok ? json_get(req->result, "site") : NULL;
    if (!json_is_object(site)) request_error_into(&v->site_error, req);
    else { set_string(&v->site_error, NULL); json_free(v->site); v->site = json_clone(site); }
    forge_site_refresh_view(v);
}

static void site_load(ForgeSite *v) {
    if (v->req_site || !store_supports("forge_site")) return;
    store_call("forge_site", site_args(v), 0, v, site_done, 0, &v->req_site);
}

static void text_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&v->error[ed], req); forge_site_refresh_view(v); return; }
    const char *content = json_str(json_get(req->result, "content"));
    set_string(&v->error[ed], NULL);
    set_string(&v->saved[ed], content ? content : "");
    if (ed == ED_SCRIPT) v->auto_source = v->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), true);
    v->loaded[ed] = true;
    forge_site_fill_editor(v, ed);
    forge_site_refresh_view(v);
}

static void text_load(ForgeSite *v, int ed) {
    const char *op = ed == ED_SCRIPT ? "forge_deploy_script" : "forge_env";
    if (v->req_load[ed] || !store_supports(op)) return;
    set_string(&v->error[ed], NULL);
    store_call(op, site_args(v), 0, v, text_done, ed, &v->req_load[ed]);
    forge_site_refresh_view(v);
}

/// What the open tab needs, read once: the script on its first showing, the .env only once asked for.
static void tab_load(ForgeSite *v) {
    if (v->tab == TAB_SCRIPT && !v->loaded[ED_SCRIPT]) text_load(v, ED_SCRIPT);
    if (v->tab == TAB_ENV && v->env_wanted && !v->loaded[ED_ENV]) text_load(v, ED_ENV);
}

// MARK: - Saving

static void save_done(void *owner, Request *req) {
    ForgeSite *v = owner;
    int ed = req->tag;
    if (!req->ok) { request_error_into(&v->error[ed], req); forge_site_refresh_view(v); return; }
    set_string(&v->error[ed], NULL);
    // What was sent is what Forge now holds; the script's answer is Forge's own word on it.
    const char *content = ed == ED_SCRIPT ? json_str(json_get(req->result, "content")) : NULL;
    const char *sent = json_str(json_get(req->args, "content"));
    set_string(&v->saved[ed], content ? content : sent ? sent : "");
    if (ed == ED_SCRIPT) v->auto_source = v->saved_auto_source = json_bool_is(json_get(req->result, "autoSource"), v->auto_source);
    forge_site_fill_editor(v, ed);
    set_string(&v->notice[ed], ed == ED_SCRIPT ? "Deploy script saved to Forge."
                                               : "Forge accepted the .env and writes it to the server shortly. Clear the config cache and restart the queue workers for running code to pick it up.");
    forge_site_refresh_view(v);
}

int forge_site_open_editor(const ForgeSite *v) { return v->tab == TAB_SCRIPT ? ED_SCRIPT : v->tab == TAB_ENV ? ED_ENV : -1; }

void forge_site_save(ForgeSite *v) {
    int ed = forge_site_open_editor(v);
    if (ed < 0 || !v->loaded[ed] || v->req_save[ed] || !forge_site_editor_changed(v, ed)) return;
    const char *op = ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env";
    if (!store_supports(op)) return;
    if (ed == ED_ENV) {
        char *title = xstrfmt("Replace the .env of %s?", site_name(v));
        bool ok = app_confirm(title, "Forge writes this file to the server in place of the current one. Running code keeps the old values until the config cache is cleared and the queue workers restart.", "Replace .env", true);
        free(title);
        if (!ok) return;
    }
    Json *args = site_args(v);
    char *text = forge_site_editor_text(v->edits[ed]);
    json_set_str(args, "content", text);
    SecureZeroMemory(text, strlen(text));
    free(text);
    if (ed == ED_SCRIPT) json_set_bool(args, "autoSource", v->auto_source);
    store_call(op, args, 0, v, save_done, ed, &v->req_save[ed]);
    forge_site_refresh_view(v);
}

void forge_site_layout(ForgeSite *v, Doc *doc, int x, int w) {
    memset(v->laid, 0, sizeof v->laid);
    const char *state = json_str_nonempty(json_get(v->site, "status"));
    doc_text(doc, x, w, site_name(v), FONT_TITLE3, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (state) { doc_space(doc, px(2)); doc_text(doc, x, w, state, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_SINGLELINE); }
    doc_space(doc, px(6));
    int h = px(40), tx = x, ty = doc->y;
    doc_tab(doc, &tx, &ty, x, x + w, h, 0xE946, "Overview", NULL, v->tab == TAB_OVERVIEW, v->base + A_TAB, TAB_OVERVIEW);
    if (store_supports("forge_deploy_script"))
        doc_tab(doc, &tx, &ty, x, x + w, h, 0xE756, v->changed[ED_SCRIPT] ? "Deploy script \xE2\x80\xA2" : "Deploy script", NULL, v->tab == TAB_SCRIPT, v->base + A_TAB, TAB_SCRIPT);
    if (store_supports("forge_env"))
        doc_tab(doc, &tx, &ty, x, x + w, h, 0xE72E, v->changed[ED_ENV] ? "Environment \xE2\x80\xA2" : "Environment", NULL, v->tab == TAB_ENV, v->base + A_TAB, TAB_ENV);
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(14));
    switch (v->tab) {
    case TAB_SCRIPT: forge_site_layout_script(v, doc, x, w); break;
    case TAB_ENV: forge_site_layout_env(v, doc, x, w); break;
    default: {
        // A wide pane would spread each label and its value apart; the overview stops at a reading width.
        int ow = w > px(820) ? px(820) : w;
        forge_site_layout_overview(v, doc, x, ow);
        break;
    }
    }
}

void forge_site_header(ForgeSite *v, HeaderInfo *info) {
    int ed = forge_site_open_editor(v);
    if (ed < 0 || !v->loaded[ed]) return;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", v->req_save[ed] ? "Saving\xE2\x80\xA6" : ed == ED_ENV ? "Save .env" : "Save");
    b->glyph = 0xE74E; b->action = v->base + A_SAVE; b->prominent = true;
    b->tip = ed == ED_ENV ? "Replace the site's .env on Forge (Ctrl+S)" : "Save the deploy script to Forge (Ctrl+S)";
    b->enabled = !v->req_save[ed] && v->changed[ed] && store_supports(ed == ED_SCRIPT ? "set_forge_deploy_script" : "set_forge_env");
}

// MARK: - Actions

void forge_site_refresh(ForgeSite *v) {
    int ed = forge_site_open_editor(v);
    if (ed < 0) { request_cancel(&v->req_site); site_load(v); forge_site_refresh_view(v); return; }
    if (ed == ED_ENV && !v->env_wanted) return;
    if (v->changed[ed] && !app_confirm("Discard unsaved changes?", "Reading it from Forge again drops the changes made here.", "Discard", true)) return;
    request_cancel(&v->req_load[ed]);
    v->loaded[ed] = false; v->changed[ed] = false;
    set_string(&v->notice[ed], NULL);
    text_load(v, ed);
}

bool forge_site_action(ForgeSite *v, int action, intptr_t arg) {
    if (!v || action < v->base || action >= v->base + FORGE_SITE_ACTIONS) return false;
    switch (action - v->base) {
    case A_TAB:
        v->tab = (int)arg;
        tab_load(v);
        forge_site_refresh_view(v);
        break;
    case A_SAVE: forge_site_save(v); break;
    case A_REVEAL: v->env_wanted = true; tab_load(v); break;
    case A_AUTO_SOURCE: v->auto_source = !v->auto_source; v->changed[ED_SCRIPT] = forge_site_editor_changed(v, ED_SCRIPT); forge_site_refresh_view(v); break;
    case A_FOCUS: if (arg >= 0 && arg < ED_COUNT && v->edits[arg]) SetFocus(v->edits[arg]); break;
    case A_OPEN_SITE: { char *url; if (forge_site_site_url(v, &url)) { open_web_url(url); free(url); } break; }
    }
    return true;
}

bool forge_site_command(ForgeSite *v, int id, int code) {
    int ed = id - ID_EDITOR;
    if (!v || ed < 0 || ed >= ED_COUNT) return false;
    if (code == EN_CHANGE) forge_site_note_change(v, ed);
    else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) pane_repaint(v->host->pane);
    return true;
}

bool forge_site_key(ForgeSite *v, WPARAM vk, bool ctrl) {
    if (!v || !ctrl || vk != 'S' || forge_site_open_editor(v) < 0) return false;
    forge_site_save(v);
    return true;
}

static void release(ForgeSite *v);

bool forge_site_can_leave(ForgeSite *v) {
    if (v && v->modal) return false;
    if (!v || (!v->changed[ED_SCRIPT] && !v->changed[ED_ENV])) return true;
    char *message = xstrfmt("The changes to %s%s%s of %s have not been saved to Forge.", v->changed[ED_SCRIPT] ? "the deploy script" : "",
                            v->changed[ED_SCRIPT] && v->changed[ED_ENV] ? " and " : "", v->changed[ED_ENV] ? "the .env" : "", site_name(v));
    v->modal = true;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    v->modal = false;
    if (v->retired) { release(v); return true; }
    if (leave) v->changed[ED_SCRIPT] = v->changed[ED_ENV] = false;
    return leave;
}

// MARK: - Lifetime

ForgeSite *forge_site_new(Screen *host, int action_base, double account, const Json *server, const Json *site) {
    ForgeSite *v = xcalloc(1, sizeof *v);
    v->host = host; v->base = action_base;
    v->account = account;
    v->server = json_clone(server);
    v->site = json_clone(site);
    json_num(json_get(server, "id"), &v->server_id);
    json_num(json_get(site, "id"), &v->site_id);
    forge_site_ensure_editors(v);
    site_load(v);
    return v;
}

static void release(ForgeSite *v) {
    for (int ed = 0; ed < ED_COUNT; ed++) {
        if (v->edits[ed]) {
            // The .env leaves the edit control's memory with the view.
            SetWindowTextW(v->edits[ed], L"");
            DestroyWindow(v->edits[ed]);
        }
        if (v->saved[ed]) SecureZeroMemory(v->saved[ed], strlen(v->saved[ed]));
        free(v->saved[ed]); free(v->error[ed]); free(v->notice[ed]);
    }
    json_free(v->server); json_free(v->site);
    free(v->site_error);
    free(v);
}

void forge_site_free(ForgeSite *v) {
    if (!v) return;
    request_cancel(&v->req_site);
    for (int ed = 0; ed < ED_COUNT; ed++) { request_cancel(&v->req_load[ed]); request_cancel(&v->req_save[ed]); }
    if (v->modal) { v->retired = true; return; }
    release(v);
}
