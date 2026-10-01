// The Servers tab, as SecureCRT's session manager: the sidebar lists the SSH servers registered in Settings, in a folder
// per project, and the detail pane holds the open sessions as tabs over a terminal. A click opens (or returns to) an SSH
// session on the server; right-click offers SFTP. Sessions run this PC's OpenSSH client (terminal.c), so its keys, agent
// and ~/.ssh/config apply, and they stay open while other screens are shown.
#include "screens.h"
#include "str.h"
#include "terminal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ACT_BACK = 1000, ACT_FOLDER, ACT_SERVER, ACT_NEW };
enum { MENU_SSH = 1, MENU_SFTP, MENU_EDIT };

typedef struct {
    Screen base;
    Json *rows;            // the server's SshServer rows
    Json *defaults;
    bool loaded;
    char *error;
    Request *req;
    char **collapsed; size_t collapsed_count;   // the projects whose folders are closed
} ServersSidebar;

static ServersSidebar *g_sidebar_screen;

static char *server_key(const Json *row) {
    double id;
    if (json_num(json_get(row, "id"), &id) && isfinite(id)) return xstrfmt("ssh:%.0f", id);
    const char *host = json_str(json_get(row, "host"));
    return xstrfmt("ssh:%s", host ? host : "");
}
/// The name a server goes by: its label, else user@host.
static char *server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}
static int server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }

/// Opens a session on a registered server in the detail pane, or returns to the one already open there.
static void connect_row(const Json *row, TermKind kind, bool reuse) {
    if (!json_is_object(row)) return;
    app_show_detail(servers_screen_new());
    Screen *root = pane_root(app_detail_pane());
    // A settings form with unsaved changes kept its place.
    if (!root || !str_eq(root->id, "servers")) return;
    char *key = server_key(row), *name = server_name(row);
    Term *open = reuse ? term_find(key, kind) : NULL;
    if (open) term_set_active(open);
    else {
        TermTarget target = { key, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), server_port(row) };
        char *error = NULL;
        if (!term_open(pane_hwnd(app_detail_pane()), kind, &target, &error)) {
            char *title = xstrfmt("Could not connect to %s", name);
            app_alert(title, error ? error : "The session could not start.");
            free(title);
        }
        free(error);
    }
    free(key); free(name);
    pane_relayout(app_detail_pane());
}

// MARK: - The sidebar

static bool is_collapsed(ServersSidebar *s, const char *repo) {
    for (size_t i = 0; i < s->collapsed_count; i++) if (str_eq(s->collapsed[i], repo)) return true;
    return false;
}
static void toggle_folder(ServersSidebar *s, const char *repo) {
    for (size_t i = 0; i < s->collapsed_count; i++)
        if (str_eq(s->collapsed[i], repo)) {
            free(s->collapsed[i]);
            s->collapsed[i] = s->collapsed[--s->collapsed_count];
            return;
        }
    s->collapsed = xrealloc(s->collapsed, (s->collapsed_count + 1) * sizeof *s->collapsed);
    s->collapsed[s->collapsed_count++] = xstrdup(repo);
}

static void rows_done(void *owner, Request *req) {
    ServersSidebar *s = owner;
    s->loaded = true;
    if (!req->ok) request_error_into(&s->error, req);
    else {
        set_string(&s->error, NULL);
        json_free(s->rows); s->rows = json_clone(json_get(req->result, "servers"));
        json_free(s->defaults); s->defaults = json_clone(json_get(req->result, "defaults"));
    }
    pane_relayout(s->base.pane);
}
static void rows_load(ServersSidebar *s) {
    if (s->req || !store_supports("settings_ssh_servers")) { s->loaded = true; return; }
    store_call("settings_ssh_servers", json_object(), 0, s, rows_done, 0, &s->req);
}
void servers_ssh_changed(void) {
    if (!g_sidebar_screen) return;
    request_cancel(&g_sidebar_screen->req);
    rows_load(g_sidebar_screen);
}
static void sidebar_terms_changed(void *ctx) { ServersSidebar *s = ctx; if (s->base.pane) pane_relayout(s->base.pane); }

/// Why the list cannot be read with this token, as a new string; NULL when it can.
static char *servers_unavailable(void) {
    if (store_supports("settings_ssh_servers")) return NULL;
    bool listed = false;
    for (size_t i = 0; i < g_store.route_count; i++) if (str_has_suffix(g_store.routes[i].path, "settings/ssh/servers")) listed = true;
    const char *permission = g_store.has_device && g_store.device.permission ? g_store.device.permission : "unknown";
    return listed
        ? xstrfmt("The SSH servers are read from Settings, which needs an Admin token, and this device's token is %s. Create an Admin token on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", permission)
        : xstrdup("This server does not offer its SSH servers (GET /settings/ssh/servers) on its client API. Update the server to list them here.");
}

static void paint_back(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    RECT t = { rc->left + px(6), rc->top, rc->right, rc->bottom };
    draw_text(cv, "\xE2\x86\x90 Back to sessions", &t, FONT_CAPTION, hovered ? theme.ink : theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

typedef struct { char *name; int count; bool open; } FolderData;
static void folder_free(void *p) { FolderData *d = p; free(d->name); free(d); }
static void paint_folder(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    FolderData *d = it->data;
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(4);
    RECT chev = { x, rc->top, x + px(16), rc->bottom };
    draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &chev, FONT_ICON_SMALL, theme.muted);
    x = chev.right + px(2);
    RECT folder = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, d->open ? 0xE838 : 0xE8B7, &folder, FONT_ICON_SMALL, theme.warning);
    char n[16]; snprintf(n, sizeof n, "%d", d->count);
    int nw = text_width(cv, n, FONT_CAPTION);
    RECT cr = { rc->right - px(8) - nw, rc->top, rc->right - px(8), rc->bottom };
    draw_text(cv, n, &cr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT t = { folder.right + px(6), rc->top, cr.left - px(8), rc->bottom };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

typedef struct { char *name, *target; int sessions; bool live, enabled; } ServerRowData;
static void server_row_free(void *p) { ServerRowData *d = p; free(d->name); free(d->target); free(d); }
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(26), top = rc->top + px(5);
    RECT icon = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, 0xE7F4, &icon, FONT_ICON_SMALL, d->live ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    if (d->sessions) {
        // A dot and the count of open sessions: green while one is connected.
        char n[16]; snprintf(n, sizeof n, "%d", d->sessions);
        int nw = text_width(cv, n, FONT_CAPTION);
        RECT cr = { right - nw, rc->top, right, rc->bottom };
        draw_text(cv, n, &cr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        draw_status_dot(cv, cr.left - px(8), (rc->top + rc->bottom) / 2, d->live ? "idle" : "closed");
        right = cr.left - px(16);
    }
    int tx = icon.right + px(6);
    RECT t = { tx, top, right, top + px(20) };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE, d->enabled ? theme.ink : theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sub = { tx, top + px(20), right, top + px(36) };
    draw_text(cv, d->target, &sub, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// The projects in the sidebar's order, then any other repository a server names, each once.
static char **folder_order(ServersSidebar *s, size_t *count) {
    size_t n = 0, cap = 8;
    char **repos = xmalloc(cap * sizeof *repos);
    size_t pc = 0;
    const Project *projects = projects_list(&pc);
    for (size_t pass = 0; pass < 2; pass++) {
        size_t limit = pass == 0 ? pc : json_count(s->rows);
        for (size_t i = 0; i < limit; i++) {
            const char *repo = pass == 0 ? projects[i].repo : json_str(json_get(json_at(s->rows, i), "repo"));
            if (!repo) repo = "";
            bool seen = false;
            for (size_t j = 0; j < n; j++) if (str_eq(repos[j], repo)) seen = true;
            if (seen) continue;
            // A project without servers has no folder.
            bool used = false;
            for (size_t j = 0; j < json_count(s->rows); j++) if (str_eq(json_str(json_get(json_at(s->rows, j), "repo")) ? json_str(json_get(json_at(s->rows, j), "repo")) : "", repo)) used = true;
            if (!used) continue;
            if (n == cap) { cap *= 2; repos = xrealloc(repos, cap * sizeof *repos); }
            repos[n++] = xstrdup(repo);
        }
    }
    *count = n;
    return repos;
}
static char *folder_name(const char *repo) {
    if (str_empty(repo)) return xstrdup("No project");
    size_t pc = 0;
    const Project *projects = projects_list(&pc);
    for (size_t i = 0; i < pc; i++) if (str_eq(projects[i].repo, repo)) return xstrdup(project_title(&projects[i]));
    const char *slash = strrchr(repo, '/');
    return xstrdup(slash ? slash + 1 : repo);
}

static void sidebar_layout(Screen *base, Doc *doc) {
    ServersSidebar *s = (ServersSidebar *)base;
    int w = doc->width;
    doc_space(doc, px(8));
    doc_custom(doc, 0, w, px(24), paint_back, NULL, NULL, ACT_BACK, 0);
    doc_space(doc, px(16));
    // The section's title, with ＋ New when this token may register a server.
    int y = doc->y, h = px(20);
    RECT tr = { px(8), y, w - px(60), y + h };
    doc_text_at(doc, &tr, "Servers", FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (store_supports("create_ssh_server")) {
        int nw = text_width(doc->cv, "\xEF\xBC\x8B New", FONT_CAPTION) + px(8);
        RECT nr = { w - px(4) - nw, y, w - px(4), y + h };
        Item *it = doc_item(doc, doc_text_at(doc, &nr, "\xEF\xBC\x8B New", FONT_CAPTION, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE));
        it->action = ACT_NEW; it->hand = true;
    }
    doc->y = y + h;
    doc_space(doc, px(6));

    char *why = servers_unavailable();
    if (why) { doc_text(doc, px(8), w - px(16), why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); return; }
    if (s->error) { doc_notice(doc, px(8), w - px(16), s->error); doc_space(doc, px(8)); }
    if (!s->loaded) { doc_loading(doc, 0, w, "Loading servers\xE2\x80\xA6"); return; }
    if (!json_count(s->rows)) {
        if (!s->error) doc_text(doc, px(8), w - px(16), "No SSH servers registered. Add one with \xEF\xBC\x8B New (or in \xE2\x9A\x99 Settings) and it is listed here, ready to connect.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    size_t folders = 0;
    char **repos = folder_order(s, &folders);
    for (size_t f = 0; f < folders; f++) {
        bool open = !is_collapsed(s, repos[f]);
        int count = 0;
        for (size_t i = 0; i < json_count(s->rows); i++) {
            const char *repo = json_str(json_get(json_at(s->rows, i), "repo"));
            if (str_eq(repo ? repo : "", repos[f])) count++;
        }
        FolderData *fd = xcalloc(1, sizeof *fd);
        fd->name = folder_name(repos[f]); fd->count = count; fd->open = open;
        doc_custom(doc, 0, w, px(30), paint_folder, fd, folder_free, ACT_FOLDER, (intptr_t)f);
        if (!open) continue;
        for (size_t i = 0; i < json_count(s->rows); i++) {
            const Json *row = json_at(s->rows, i);
            const char *repo = json_str(json_get(row, "repo"));
            if (!str_eq(repo ? repo : "", repos[f])) continue;
            ServerRowData *d = xcalloc(1, sizeof *d);
            d->name = server_name(row);
            const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
            d->target = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", server_port(row));
            d->enabled = !json_bool_is(json_get(row, "enabled"), false);
            char *key = server_key(row);
            d->sessions = (int)term_count_for(key);
            for (size_t t = 0; t < term_count(); t++) if (str_eq(term_key(term_at(t)), key) && term_running(term_at(t))) d->live = true;
            free(key);
            doc_custom(doc, 0, w, px(46), paint_server, d, server_row_free, ACT_SERVER, (intptr_t)i);
        }
        doc_space(doc, px(4));
    }
    str_array_free(repos, folders);
    doc_space(doc, px(8));
    doc_text(doc, px(8), w - px(16), "Click a server for an SSH session; right-click it for SFTP.", FONT_CAPTION2, theme.tertiary, DT_WORDBREAK);
}

static void sidebar_header(Screen *base, HeaderInfo *info) { (void)base; (void)info; }

static void sidebar_back(ServersSidebar *s) { pane_pop(s->base.pane); }
/// The repository of the folder at an index of `folder_order`, as a new string; NULL past the end.
static char *folder_repo(ServersSidebar *s, size_t f) {
    size_t folders = 0;
    char **repos = folder_order(s, &folders);
    char *repo = f < folders ? xstrdup(repos[f]) : NULL;
    str_array_free(repos, folders);
    return repo;
}
static void sidebar_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ServersSidebar *s = (ServersSidebar *)base;
    switch (action) {
    case ACT_BACK: sidebar_back(s); break;
    case ACT_FOLDER: {
        char *repo = folder_repo(s, (size_t)arg);
        if (repo) { toggle_folder(s, repo); free(repo); pane_relayout(base->pane); }
        break;
    }
    case ACT_SERVER: connect_row(json_at(s->rows, (size_t)arg), TERM_SSH, true); break;
    case ACT_NEW: app_show_detail(ssh_server_settings_screen_new(NULL, s->defaults)); break;
    }
}
static void sidebar_context(Screen *base, int action, intptr_t arg, POINT pt) {
    ServersSidebar *s = (ServersSidebar *)base;
    if (action != ACT_SERVER) return;
    const Json *row = json_at(s->rows, (size_t)arg);
    if (!json_is_object(row)) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_SSH, L"Connect SSH in a new tab");
    AppendMenuW(menu, MF_STRING, MENU_SFTP, L"Connect SFTP tab");
    if (store_supports("update_ssh_server")) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING, MENU_EDIT, L"Edit server\xE2\x80\xA6" + 0);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(base->pane), NULL);
    DestroyMenu(menu);
    if (chosen == MENU_SSH) connect_row(row, TERM_SSH, false);
    else if (chosen == MENU_SFTP) connect_row(row, TERM_SFTP, false);
    else if (chosen == MENU_EDIT) app_show_detail(ssh_server_settings_screen_new(row, s->defaults));
}
static void sidebar_visible(Screen *base, bool shown) {
    ServersSidebar *s = (ServersSidebar *)base;
    if (shown && !s->loaded && !s->req) rows_load(s);
}
static void sidebar_refresh(Screen *base) {
    ServersSidebar *s = (ServersSidebar *)base;
    request_cancel(&s->req);
    rows_load(s);
}
static bool sidebar_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)ctrl; (void)shift;
    if (vk == VK_BACK || vk == VK_ESCAPE) { sidebar_back((ServersSidebar *)base); return true; }
    return false;
}
static void sidebar_destroy(Screen *base) {
    ServersSidebar *s = (ServersSidebar *)base;
    if (g_sidebar_screen == s) g_sidebar_screen = NULL;
    term_remove_listener(sidebar_terms_changed, s);
    request_cancel(&s->req);
    json_free(s->rows); json_free(s->defaults); free(s->error);
    str_array_free(s->collapsed, s->collapsed_count);
    screen_release(base);
}

static const ScreenVTable sidebar_vt = {
    .destroy = sidebar_destroy, .layout = sidebar_layout, .header = sidebar_header, .action = sidebar_action,
    .context = sidebar_context, .visible = sidebar_visible, .refresh = sidebar_refresh, .key = sidebar_key,
};
Screen *servers_sidebar_new(void) {
    ServersSidebar *s = xcalloc(1, sizeof *s);
    s->base.vt = &sidebar_vt; s->base.id = xstrdup("servers-sidebar");
    s->rows = json_array();
    g_sidebar_screen = s;
    term_add_listener(sidebar_terms_changed, s);
    return &s->base;
}

// MARK: - The sessions

enum { ACT_TAB = 1100, ACT_TAB_CLOSE, ACT_RECONNECT, ACT_CLOSE, ACT_OTHER_KIND };

typedef struct {
    Screen base;
    RECT term_rc;          // the terminal's area, in content coordinates
    bool shown;
    Term *focused;         // the session last given the keyboard
} ServersScreen;

typedef struct { char *label; bool active, live, sftp; } TabData;
static void tab_free(void *p) { TabData *d = p; free(d->label); free(d); }
enum { TAB_H = 30, TAB_CLOSE_W = 22 };
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    RECT box = *rc;
    if (d->active || hovered) fill_round_rect(cv, &box, px(6), d->active ? theme.raise : blend(theme.raise, theme.canvas, 0.5), d->active ? theme.line : blend(theme.raise, theme.canvas, 0.5));
    if (d->active) { RECT bar = { rc->left + px(8), rc->bottom - px(2), rc->right - px(8), rc->bottom }; fill_rect(cv, &bar, theme.accent); }
    int x = rc->left + px(10), cy = (rc->top + rc->bottom) / 2;
    draw_status_dot(cv, x + px(3), cy, d->live ? "idle" : "closed");
    x += px(12);
    int right = rc->right - px(TAB_CLOSE_W);
    if (d->sftp) {
        int bh = 0, bw = draw_badge(NULL, 0, 0, 0, "SFTP", theme.accent, theme.raise, &bh);
        draw_badge(cv, right - bw - px(4), cy - bh / 2, 0, "SFTP", theme.accent, d->active ? theme.raise : theme.canvas, &bh);
        right -= bw + px(8);
    }
    RECT t = { x, rc->top, right, rc->bottom };
    draw_text(cv, d->label, &t, d->active ? FONT_CAPTION_SEMIBOLD : FONT_CAPTION, d->active ? theme.ink : theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_tab_close(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(4), theme.line, theme.line);
    draw_glyph(cv, 0xE711, rc, FONT_ICON_SMALL, hovered ? theme.ink : theme.muted);
}

static void tabs_layout(ServersScreen *s, Doc *doc, int w) {
    int x = 0, y = doc->y, h = px(TAB_H), gap = px(4);
    Term *active = term_active();
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        TabData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(term_label(t)); d->active = t == active; d->live = term_running(t); d->sftp = term_kind(t) == TERM_SFTP;
        int tw = text_width(doc->cv, d->label, FONT_CAPTION_SEMIBOLD) + px(10 + 12 + TAB_CLOSE_W + 6) + (d->sftp ? px(44) : 0);
        if (tw > px(240)) tw = px(240);
        if (tw < px(110)) tw = px(110);
        if (x > 0 && x + tw > w) { x = 0; y += h + gap; }
        RECT tr = { x, y, x + tw, y + h };
        int ti = doc_add(doc, &tr, paint_tab);
        Item *it = doc_item(doc, ti);
        it->data = d; it->free_data = tab_free; it->action = ACT_TAB; it->arg = (intptr_t)i; it->hand = true;
        RECT cr = { tr.right - px(TAB_CLOSE_W) - px(2), y + (h - px(18)) / 2, tr.right - px(4), y + (h - px(18)) / 2 + px(18) };
        int ci = doc_add(doc, &cr, paint_tab_close);
        Item *c = doc_item(doc, ci);
        c->action = ACT_TAB_CLOSE; c->arg = (intptr_t)i; c->hand = true;
        x += tw + gap;
    }
    doc->y = y + h;
    (void)s;
}

static void servers_layout(Screen *base, Doc *doc) {
    ServersScreen *s = (ServersScreen *)base;
    int w = doc->width;
    SetRectEmpty(&s->term_rc);
    doc_space(doc, px(10));
    if (!term_count()) {
        doc_space(doc, px(40));
        doc_empty_state(doc, 0, w, 0xE7F4, "No open sessions",
                        "Click a server on the left to open an SSH session in a tab here, or right-click it for an SFTP tab. "
                        "Sessions use this PC\xE2\x80\x99s OpenSSH client, so your keys, ssh-agent and ~/.ssh/config apply, and password prompts appear in the terminal.");
        return;
    }
    tabs_layout(s, doc, w);
    doc_space(doc, px(8));
    // The terminal fills the rest of the pane, so nothing scrolls.
    RECT view = pane_content_rect(base->pane);
    int top = doc->y, h = (view.bottom - view.top) - top - px(14);
    if (h < px(120)) h = px(120);
    SetRect(&s->term_rc, 0, top, w, top + h);
    doc->y = top + h;
}

static void servers_place(Screen *base, const RECT *content, int scroll_y) {
    ServersScreen *s = (ServersScreen *)base;
    Term *active = term_active();
    RECT r = { 0 };
    bool on = s->shown && active && !IsRectEmpty(&s->term_rc);
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(base->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(base->pane)) / 2;
        SetRect(&r, content->left + m + s->term_rc.left, content->top + s->term_rc.top - scroll_y, content->left + m + s->term_rc.right, content->top + s->term_rc.bottom - scroll_y);
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else r = visible;
    }
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        term_place(t, &r, on && t == active);
    }
    // A session just opened or switched to takes the keyboard.
    if (on && s->focused != active) { s->focused = active; term_focus(active); }
}

static void servers_header(Screen *base, HeaderInfo *info) {
    (void)base;
    Term *t = term_active();
    if (!t) {
        snprintf(info->title, sizeof info->title, "Servers");
        snprintf(info->subtitle, sizeof info->subtitle, "SSH and SFTP sessions from this PC");
        return;
    }
    snprintf(info->title, sizeof info->title, "%s", term_label(t));
    const char *title = term_title(t);
    snprintf(info->subtitle, sizeof info->subtitle, "%s %s \xC2\xB7 %s%s%s", term_kind(t) == TERM_SFTP ? "SFTP" : "SSH", term_target(t),
             term_running(t) ? "connected" : "closed", title && *title ? " \xC2\xB7 " : "", title && *title ? title : "");
    snprintf(info->status, sizeof info->status, "%s", term_running(t) ? "idle" : "closed");
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = term_kind(t) == TERM_SFTP ? 0xE756 : 0xE8B7; b->action = ACT_OTHER_KIND; b->enabled = true;
    b->tip = term_kind(t) == TERM_SFTP ? "Open an SSH session on this server" : "Open an SFTP session on this server";
    snprintf(b->label, sizeof b->label, "%s", term_kind(t) == TERM_SFTP ? "SSH" : "SFTP");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE72C; b->action = ACT_RECONNECT; b->enabled = !term_running(t); b->tip = "Reconnect";
    snprintf(b->label, sizeof b->label, "Reconnect");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE711; b->action = ACT_CLOSE; b->enabled = true; b->tip = "Close this session";
}

static void close_term(Term *t) {
    if (!t) return;
    if (term_running(t)) {
        char *title = xstrfmt("Disconnect from %s?", term_label(t));
        bool ok = app_confirm(title, "The session ends and anything running in it on the server is stopped with it.", "Disconnect", true);
        free(title);
        if (!ok) return;
    }
    term_close(t);
}
static void servers_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)pt;
    ServersScreen *s = (ServersScreen *)base;
    switch (action) {
    case ACT_TAB: term_set_active(term_at((size_t)arg)); break;
    case ACT_TAB_CLOSE: close_term(term_at((size_t)arg)); break;
    case ACT_CLOSE: close_term(term_active()); break;
    case ACT_RECONNECT: {
        Term *t = term_active();
        char *error = NULL;
        if (t && !term_reconnect(t, &error)) app_alert("Could not reconnect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    case ACT_OTHER_KIND: {
        Term *t = term_active();
        if (!t) break;
        // The same server, the other client.
        TermTarget tt;
        term_get_target(t, &tt);
        char *error = NULL;
        if (!term_open(pane_hwnd(base->pane), term_kind(t) == TERM_SFTP ? TERM_SSH : TERM_SFTP, &tt, &error)) app_alert("Could not connect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    }
    (void)s;
}

static void servers_terms_changed(void *ctx) {
    ServersScreen *s = ctx;
    if (!s->base.pane) return;
    pane_relayout(s->base.pane);
    pane_header_changed(s->base.pane);
    // The 🖥 strip button counts the open tabs.
    pane_relayout(app_sidebar_pane());
}
static void servers_visible(Screen *base, bool shown) {
    ServersScreen *s = (ServersScreen *)base;
    s->shown = shown;
    s->focused = NULL;
    if (!shown) for (size_t i = 0; i < term_count(); i++) term_place(term_at(i), NULL, false);
    else pane_relayout(base->pane);
}
static void servers_destroy(Screen *base) {
    ServersScreen *s = (ServersScreen *)base;
    term_remove_listener(servers_terms_changed, s);
    // The sessions go on; their windows wait, hidden, for the tab to come back.
    for (size_t i = 0; i < term_count(); i++) term_place(term_at(i), NULL, false);
    screen_release(base);
}

static const ScreenVTable servers_vt = {
    .destroy = servers_destroy, .layout = servers_layout, .header = servers_header, .action = servers_action,
    .place = servers_place, .visible = servers_visible,
};
Screen *servers_screen_new(void) {
    ServersScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &servers_vt; s->base.id = xstrdup("servers");
    term_add_listener(servers_terms_changed, s);
    return &s->base;
}
