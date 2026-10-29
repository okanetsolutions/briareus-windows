#include "project_ssh_internal.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>

// MARK: - Layout

typedef struct { char *name, *target; int sessions; bool live, enabled, selected; } ServerRowData;
static void server_row_free(void *v) { ServerRowData *d = v; free(d->name); free(d->target); free(d); }
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (d->selected || doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(7), theme.raise, d->selected ? theme.accent_dim : theme.raise);
    int x = rc->left + px(6), top = rc->top + px(9);
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
    draw_text(cv, d->target, &sub, FONT_CAPTION, theme.secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

void project_ssh_layout_server(ProjectSsh *p, Doc *doc, int x, int w, size_t i) {
    const Json *row = json_at(p->rows, i);
    ServerRowData *d = xcalloc(1, sizeof *d);
    d->name = ssh_server_name(row);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    d->target = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", project_ssh_server_port(row));
    d->enabled = !json_bool_is(json_get(row, "enabled"), false);
    char *key = ssh_server_key(row);
    d->sessions = (int)term_count_for(key);
    for (size_t t = 0; t < term_count(); t++) if (str_eq(term_key(term_at(t)), key) && term_running(term_at(t))) d->live = true;
    Term *shown = project_ssh_shown_term(p);
    d->selected = shown && str_eq(term_key(shown), key);
    free(key);
    doc_custom(doc, x, w, px(58), paint_server, d, server_row_free, p->base + A_SERVER, (intptr_t)i);
}

/// The servers down the left, each with its name, user@host:port and its open sessions.
static void layout_list(ProjectSsh *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading servers\xE2\x80\xA6"); return; }
    if (p->global) { project_ssh_global_list(p, doc, w); return; }
    for (size_t i = 0; i < json_count(p->rows); i++) project_ssh_layout_server(p, doc, x, w, i);
    if (!json_count(p->rows) && !p->error) {
        doc_text(doc, x + px(6), w - px(12), "No SSH servers for this project. Register one under \xE2\x9A\x99 Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
}

/// The project's sessions as tabs, wrapping, from `x` across `w`.
static void layout_tabs(ProjectSsh *p, Doc *doc, int x0, int w) {
    int x = x0, y = doc->y, h = px(TAB_H), gap = px(4);
    Term *shown = project_ssh_shown_term(p);
    for (size_t i = 0; i < term_count(); i++) {
        Term *t = term_at(i);
        if (!p->global && !str_eq(term_group(t), p->repo)) continue;
        TabData *d = xcalloc(1, sizeof *d);
        d->label = project_ssh_tab_label(p, t); d->active = t == shown; d->live = term_running(t);
        int tw = text_width(doc->cv, d->label, FONT_CAPTION_SEMIBOLD) + px(10 + 12 + TAB_CLOSE_W + 6);
        if (tw > w) tw = w;
        if (tw > px(240)) tw = px(240);
        if (tw < px(110) && w >= px(110)) tw = px(110);
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
    servers_ssh_scope_changed();
    SetRectEmpty(&p->term_rc);
    if (!project_ssh_offered()) {
        doc_text(doc, 0, w, "The SSH servers are read from Settings, which needs an Admin token. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_ssh_load(p);
    RECT view = pane_content_rect(p->host->pane);
    doc_space(doc, px(16));
    int top = doc->y, bottom = top + (view.bottom - view.top) - top - px(14);
    if (bottom < top) bottom = top;
    int lw = p->global ? px(320) : px(LIST_W);
    if (lw > w * 2 / 5) lw = w * 2 / 5;
    int first = (int)doc->count;
    layout_list(p, doc, 0, lw - px(10));
    RECT list_view = { 0, top, lw, bottom };
    doc_region(doc, first, (int)doc->count, &list_view, doc->y);
    // A rule between the list and the sessions.
    RECT rule = { lw + px(8), top, lw + px(9), bottom };
    doc_add(doc, &rule, paint_rule);
    int rx = lw + px(18), rw = w - rx;
    doc->y = top;
    if (!(p->global ? term_count() : project_ssh_session_count(p->repo))) {
        doc_space(doc, px(40));
        doc_empty_state(doc, rx, rw, 0xE7F4, "No open sessions",
                        "Click a server on the left to open an SSH session here. It connects from this PC straight to the server with your own "
                        "OpenSSH client, so your keys, ssh-agent and ~/.ssh/config apply, and password prompts appear in the terminal.");
    } else {
        layout_tabs(p, doc, rx, rw);
        doc_space(doc, px(8));
        SetRect(&p->term_rc, rx, doc->y, w, bottom);
    }
    doc->y = bottom;
}

void project_ssh_header(ProjectSsh *p, HeaderInfo *info) {
    if (!project_ssh_current(p) || !project_ssh_offered()) return;
    Term *t = project_ssh_shown_term(p);
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
