// A project's SSH sessions tab, beside its pull requests and issues: the SSH servers registered for the project in Settings
// down the left, and its open sessions as tabs over a terminal on the right. A click on a server opens an SSH session to it
// (or returns to the open one). Sessions run this PC's OpenSSH client
// (terminal.c) straight to the server, so its keys, agent and ~/.ssh/config apply, and they stay open while other screens
// are shown.
#include "screens.h"
#include "str.h"
#include "terminal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_SERVER, A_TAB, A_TAB_CLOSE, A_RECONNECT, A_CLOSE };
enum { LIST_W = 260, TAB_H = 30, TAB_CLOSE_W = 22, MAX_INSTANCES = 16 };

struct ProjectSsh {
    char *repo;
    Screen *host;
    int base;
    Json *rows;            // the project's SshServer rows
    bool loaded, loading;
    char *error;
    Request *req;
    RECT term_rc;          // the terminal's area, in content coordinates
    Term *focused;         // the session last given the keyboard
};

static ProjectSsh *g_instances[MAX_INSTANCES];

bool project_ssh_offered(void) { return store_supports("settings_ssh_servers"); }

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

/// The project's session on show: the active one when it is the project's, else its first.
static Term *shown_term(ProjectSsh *p) {
    Term *active = term_active();
    if (active && str_eq(term_group(active), p->repo)) return active;
    for (size_t i = 0; i < term_count(); i++) if (str_eq(term_group(term_at(i)), p->repo)) return term_at(i);
    return NULL;
}
size_t project_ssh_session_count(const char *repo) {
    size_t n = 0;
    for (size_t i = 0; i < term_count(); i++) if (str_eq(term_group(term_at(i)), repo)) n++;
    return n;
}

static void relayout(ProjectSsh *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static void terms_changed(void *ctx) { relayout(ctx); }

// MARK: - Loading

static void rows_done(void *owner, Request *req) {
    ProjectSsh *p = owner;
    p->loaded = true; p->loading = false;
    if (!req->ok) request_error_into(&p->error, req);
    else {
        set_string(&p->error, NULL);
        json_free(p->rows); p->rows = json_array();
        const Json *all = json_get(req->result, "servers");
        for (size_t i = 0; i < json_count(all); i++)
            if (str_eq(json_str(json_get(json_at(all, i), "repo")), p->repo)) json_array_push(p->rows, json_clone(json_at(all, i)));
    }
    relayout(p);
}
void project_ssh_load(ProjectSsh *p) {
    if (p->loaded || p->req) return;
    if (!project_ssh_offered()) { p->loaded = true; return; }
    p->loading = true;
    store_call("settings_ssh_servers", json_object(), 0, p, rows_done, 0, &p->req);
}
void project_ssh_refresh(ProjectSsh *p) {
    request_cancel(&p->req);
    p->loaded = false;
    project_ssh_load(p);
}
void servers_ssh_changed(void) {
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] && (g_instances[i]->loaded || g_instances[i]->req)) project_ssh_refresh(g_instances[i]);
    servers_sftp_changed();
}

// MARK: - Connecting

static void connect_row(ProjectSsh *p, const Json *row) {
    if (!json_is_object(row) || !p->host->pane) return;
    char *key = server_key(row), *name = server_name(row);
    Term *open = term_find(key);
    if (open) term_set_active(open);
    else {
        TermTarget target = { key, p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), server_port(row) };
        char *error = NULL;
        if (!term_open(pane_hwnd(p->host->pane), &target, &error)) {
            char *title = xstrfmt("Could not connect to %s", name);
            app_alert(title, error ? error : "The session could not start.");
            free(title);
        }
        free(error);
    }
    free(key); free(name);
    relayout(p);
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

// MARK: - Layout

typedef struct { char *name, *target; int sessions; bool live, enabled; } ServerRowData;
static void server_row_free(void *v) { ServerRowData *d = v; free(d->name); free(d->target); free(d); }
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(6), top = rc->top + px(5);
    RECT icon = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, 0xE7F4, &icon, FONT_ICON_SMALL, d->live ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    if (d->sessions) {
        // A dot and how many sessions are open: green while one is connected.
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

static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_rect(cv, rc, theme.line); }

typedef struct { char *label; bool active, live; } TabData;
static void tab_free(void *v) { TabData *d = v; free(d->label); free(d); }
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    TabData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    COLORREF soft = blend(theme.raise, theme.canvas, 0.5);
    if (d->active || hovered) fill_round_rect(cv, rc, px(6), d->active ? theme.raise : soft, d->active ? theme.line : soft);
    if (d->active) { RECT bar = { rc->left + px(8), rc->bottom - px(2), rc->right - px(8), rc->bottom }; fill_rect(cv, &bar, theme.accent); }
    int x = rc->left + px(10), cy = (rc->top + rc->bottom) / 2;
    draw_status_dot(cv, x + px(3), cy, d->live ? "idle" : "closed");
    RECT t = { x + px(12), rc->top, rc->right - px(TAB_CLOSE_W), rc->bottom };
    draw_text(cv, d->label, &t, d->active ? FONT_CAPTION_SEMIBOLD : FONT_CAPTION, d->active ? theme.ink : theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void paint_tab_close(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    bool hovered = doc_item_hovered(doc, it);
    if (hovered) fill_round_rect(cv, rc, px(4), theme.line, theme.line);
    draw_glyph(cv, 0xE711, rc, FONT_ICON_SMALL, hovered ? theme.ink : theme.muted);
}

/// The servers down the left, each with its name, user@host:port and its open sessions.
static void layout_list(ProjectSsh *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading servers\xE2\x80\xA6"); return; }
    for (size_t i = 0; i < json_count(p->rows); i++) {
        const Json *row = json_at(p->rows, i);
        ServerRowData *d = xcalloc(1, sizeof *d);
        d->name = server_name(row);
        const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
        d->target = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", server_port(row));
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        char *key = server_key(row);
        d->sessions = (int)term_count_for(key);
        for (size_t t = 0; t < term_count(); t++) if (str_eq(term_key(term_at(t)), key) && term_running(term_at(t))) d->live = true;
        free(key);
        doc_custom(doc, x, w, px(46), paint_server, d, server_row_free, p->base + A_SERVER, (intptr_t)i);
    }
    if (!json_count(p->rows) && !p->error) {
        doc_text(doc, x + px(6), w - px(12), "No SSH servers for this project. Register one under \xE2\x9A\x99 Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
}

/// The project's sessions as tabs, wrapping, from `x` across `w`.
static void layout_tabs(ProjectSsh *p, Doc *doc, int x0, int w) {
    int x = x0, y = doc->y, h = px(TAB_H), gap = px(4);
    Term *shown = shown_term(p);
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        if (!str_eq(term_group(t), p->repo)) continue;
        TabData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(term_label(t)); d->active = t == shown; d->live = term_running(t);
        int tw = text_width(doc->cv, d->label, FONT_CAPTION_SEMIBOLD) + px(10 + 12 + TAB_CLOSE_W + 6);
        if (tw > px(240)) tw = px(240);
        if (tw < px(110)) tw = px(110);
        if (x > x0 && x + tw > x0 + w) { x = x0; y += h + gap; }
        RECT tr = { x, y, x + tw, y + h };
        Item *it = doc_item(doc, doc_add(doc, &tr, paint_tab));
        it->data = d; it->free_data = tab_free; it->action = p->base + A_TAB; it->arg = (intptr_t)i; it->hand = true;
        RECT cr = { tr.right - px(TAB_CLOSE_W) - px(2), y + (h - px(18)) / 2, tr.right - px(4), y + (h - px(18)) / 2 + px(18) };
        Item *c = doc_item(doc, doc_add(doc, &cr, paint_tab_close));
        c->action = p->base + A_TAB_CLOSE; c->arg = (intptr_t)i; c->hand = true;
        x += tw + gap;
    }
    doc->y = y + h;
}

void project_ssh_layout(ProjectSsh *p, Doc *doc, int w) {
    SetRectEmpty(&p->term_rc);
    if (!project_ssh_offered()) {
        doc_text(doc, 0, w, "The SSH servers are read from Settings, which needs an Admin token. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_ssh_load(p);
    RECT view = pane_content_rect(p->host->pane);
    int top = doc->y, bottom = top + (view.bottom - view.top) - top - px(14);
    if (bottom < top + px(240)) bottom = top + px(240);
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    layout_list(p, doc, 0, lw);
    int list_bottom = doc->y;
    // A rule between the list and the sessions.
    RECT rule = { lw + px(8), top, lw + px(9), bottom };
    doc_add(doc, &rule, paint_rule);
    int rx = lw + px(18), rw = w - rx;
    doc->y = top;
    if (!project_ssh_session_count(p->repo)) {
        doc_space(doc, px(40));
        doc_empty_state(doc, rx, rw, 0xE7F4, "No open sessions",
                        "Click a server on the left to open an SSH session here. It connects from this PC straight to the server with your own "
                        "OpenSSH client, so your keys, ssh-agent and ~/.ssh/config apply, and password prompts appear in the terminal.");
    } else {
        layout_tabs(p, doc, rx, rw);
        doc_space(doc, px(8));
        SetRect(&p->term_rc, rx, doc->y, w, bottom);
    }
    doc->y = bottom > list_bottom ? bottom : list_bottom;
}

void project_ssh_header(ProjectSsh *p, HeaderInfo *info) {
    Term *t = shown_term(p);
    if (!t) return;
    const char *title = term_title(t);
    snprintf(info->subtitle, sizeof info->subtitle, "SSH %s \xC2\xB7 %s%s%s", term_target(t), term_running(t) ? "connected" : "closed",
             title && *title ? " \xC2\xB7 " : "", title && *title ? title : "");
    snprintf(info->status, sizeof info->status, "%s", term_running(t) ? "idle" : "closed");
    HeaderButton *b = &info->buttons[info->button_count++];
    b->glyph = 0xE72C; b->action = p->base + A_RECONNECT; b->enabled = !term_running(t); b->tip = "Connect this session again";
    snprintf(b->label, sizeof b->label, "Reconnect");
    b = &info->buttons[info->button_count++];
    b->glyph = 0xE711; b->action = p->base + A_CLOSE; b->enabled = true; b->tip = "Close this session";
}

void project_ssh_place(ProjectSsh *p, const RECT *content, int scroll_y, bool shown) {
    Term *active = shown ? shown_term(p) : NULL;
    RECT r = { 0 };
    bool on = active && content && !IsRectEmpty(&p->term_rc);
    if (on) {
        RECT rc; GetClientRect(pane_hwnd(p->host->pane), &rc);
        int m = (rc.right - rc.left - pane_content_width(p->host->pane)) / 2;
        SetRect(&r, content->left + m + p->term_rc.left, content->top + p->term_rc.top - scroll_y, content->left + m + p->term_rc.right, content->top + p->term_rc.bottom - scroll_y);
        RECT visible;
        if (!IntersectRect(&visible, &r, content)) on = false;
        else r = visible;
    }
    // Only this project's session on show is up; every other one waits, hidden.
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        if (t == active && on) term_place(t, &r, true);
        else if (str_eq(term_group(t), p->repo) || !shown) term_place(t, NULL, false);
    }
    if (!on) { p->focused = NULL; return; }
    // A session just opened or switched to takes the keyboard.
    if (p->focused != active) { p->focused = active; term_focus(active); }
}

// MARK: - Actions

static Term *term_by_index(intptr_t arg) { return arg >= 0 ? term_at((size_t)arg) : NULL; }
bool project_ssh_action(ProjectSsh *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_SSH_ACTIONS) return false;
    switch (action - p->base) {
    case A_SERVER: connect_row(p, json_at(p->rows, (size_t)arg)); break;
    case A_TAB: term_set_active(term_by_index(arg)); break;
    case A_TAB_CLOSE: close_term(term_by_index(arg)); break;
    case A_CLOSE: close_term(shown_term(p)); break;
    case A_RECONNECT: {
        Term *t = shown_term(p);
        char *error = NULL;
        if (t && !term_reconnect(t, &error)) app_alert("Could not reconnect", error ? error : "The session could not start.");
        free(error);
        break;
    }
    }
    return true;
}

// MARK: - Lifetime

ProjectSsh *project_ssh_new(const char *repo, Screen *host, int action_base) {
    ProjectSsh *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo ? repo : "");
    p->host = host; p->base = action_base;
    p->rows = json_array();
    for (int i = 0; i < MAX_INSTANCES; i++) if (!g_instances[i]) { g_instances[i] = p; break; }
    term_add_listener(terms_changed, p);
    return p;
}
void project_ssh_free(ProjectSsh *p) {
    if (!p) return;
    for (int i = 0; i < MAX_INSTANCES; i++) if (g_instances[i] == p) g_instances[i] = NULL;
    term_remove_listener(terms_changed, p);
    // The sessions go on; their windows wait, hidden, for the tab to come back.
    for (size_t i = 0; i < term_count(); i++) if (str_eq(term_group(term_at(i)), p->repo)) term_place(term_at(i), NULL, false);
    request_cancel(&p->req);
    json_free(p->rows);
    free(p->repo); free(p->error);
    free(p);
}
