#include "screen_settings_github_internal.h"

bool screen_settings_github_offered(void) {
    return g_store.has_device && str_eq(g_store.device.permission, "admin") && store_supports("settings_github_accounts");
}

bool screen_settings_github_current(GithubForm *s) {
    return s->client == g_store.client && str_eq(s->device, g_store.device.id) && screen_settings_github_offered();
}

char *screen_settings_github_form_id(double id) { return id ? xstrfmt("settings-github:%.0f", id) : xstrdup("settings-github:new"); }

static void github_done(void *owner, Request *req) {
    SettingsScreen *s = owner;
    if (req->client != s->github_client || req->client != g_store.client) return;
    if (!str_eq(s->github_device, g_store.device.id) || !screen_settings_github_offered()) return;
    s->github_loaded = true;
    if (!req->ok || !json_is_array(json_get(req->result, "accounts"))) {
        char *error = request_error_or_unexpected(req);
        set_string(&s->github_error, error); free(error);
    } else {
        set_string(&s->github_error, NULL);
        json_free(s->github); s->github = json_clone(req->result);
        Json *accounts = (Json *)json_get(s->github, "accounts");
        for (size_t i = 0; i < json_count(accounts); i++) json_object_remove((Json *)json_at(accounts, i), "token");
    }
    pane_relayout(s->base.pane);
}

void screen_settings_github_load(SettingsScreen *s) {
    if (s->github_client != g_store.client || !str_eq(s->github_device, g_store.device.id)) {
        request_cancel(&s->req_github); api_client_release(s->github_client);
        s->github_client = api_client_retain(g_store.client); set_string(&s->github_device, g_store.device.id);
        json_free(s->github); s->github = NULL; s->github_loaded = false; set_string(&s->github_error, NULL);
    }
    if (s->req_github || !screen_settings_github_offered()) return;
    store_call("settings_github_accounts", json_object(), 0, s, github_done, 0, &s->req_github);
}

void screen_settings_github_changed_list(void) {
    SettingsScreen *s = screen_settings_g_settings;
    if (!s) return;
    request_cancel(&s->req_github); screen_settings_github_load(s);
}

void screen_settings_github_open(SettingsScreen *s, intptr_t index) {
    if (!screen_settings_github_offered() || s->github_client != g_store.client) return;
    if (!str_eq(s->github_device, g_store.device.id)) return;
    if (index < 0) {
        if (store_supports("create_github_account")) app_show_detail(screen_settings_github_settings_screen_new(NULL, NULL));
        return;
    }
    const Json *row = json_at(json_get(s->github, "accounts"), (size_t)index);
    if (json_is_object(row)) app_show_detail(screen_settings_github_settings_screen_new(row, NULL));
}

typedef struct { char *label, *login, *projects, *scopes; bool selected; } GithubRow;

static void github_row_free(void *p) {
    GithubRow *d = p; free(d->label); free(d->login); free(d->projects); free(d->scopes); free(d);
}

static void paint_github_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    GithubRow *d = it->data;
    if (d->selected || doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    const char *lines[] = {d->label, d->login, d->projects, d->scopes};
    int y = rc->top + px(6);
    for (size_t i = 0; i < sizeof lines / sizeof *lines; i++) {
        int h = px(i ? 18 : 22);
        RECT line = {rc->left + px(8), y, rc->right - px(8), y + h};
        draw_text(cv, lines[i], &line, i ? FONT_CAPTION : FONT_SUBHEADLINE, i ? theme.muted : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += h;
    }
}

void screen_settings_github_section(SettingsScreen *s, Doc *doc, int w, const char *selected) {
    if (!screen_settings_github_offered()) return;
    doc_space(doc, px(8));
    screen_settings_section_title(doc, w, "GitHub accounts", NULL, store_supports("create_github_account") ? ACT_NEW_GITHUB : 0);
    if (s->github_error) doc_notice(doc, px(8), w - px(16), s->github_error);
    if (s->github_client != g_store.client || !s->github_loaded) { doc_loading(doc, 0, w, "Loading GitHub accounts…"); return; }
    const Json *rows = json_get(s->github, "accounts");
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *row = json_at(rows, i);
        const char *login = json_str_or(json_get(row, "login"), "");
        GithubRow *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(json_str_or(json_get(row, "label"), login));
        if (str_empty(d->label)) set_string(&d->label, login);
        d->login = xstrfmt("Verified login: %s", login);
        d->projects = github_account_projects(row); d->scopes = github_account_scopes(row);
        char *id = screen_settings_github_form_id(screen_settings_ssh_row_id(row));
        d->selected = str_eq(selected, id); free(id);
        int item = doc_custom(doc, 0, w, px(88), paint_github_row, d, github_row_free, ACT_OPEN_GITHUB, (intptr_t)i);
        doc_item(doc, item)->tip = xstrfmt("%s\n%s\n%s", d->login, d->projects, d->scopes);
    }
    if (!json_count(rows) && !s->github_error) doc_text(doc, px(8), w - px(16), "No GitHub accounts yet.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}
