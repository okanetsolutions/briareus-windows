#include "project_sftp_internal.h"

// MARK: - Layout

typedef struct { char *name, *target; int sessions; bool live, enabled; } ServerRowData;

typedef struct { char *label; bool active, live; } TabData;

static void server_row_free(void *v);
static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void tab_free(void *v);
static void paint_tab(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_tab_close(Doc *doc, Item *it, Canvas *cv, const RECT *rc);

static void server_row_free(void *v) { ServerRowData *d = v; free(d->name); free(d->target); free(d); }

static void paint_server(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    ServerRowData *d = it->data;
    if (doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(6), top = rc->top + px(5);
    RECT icon = { x, rc->top, x + px(20), rc->bottom };
    draw_glyph(cv, 0xE8B7, &icon, FONT_ICON_SMALL, d->live ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    if (d->sessions) {
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

void project_sftp_paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) { (void)doc; (void)it; fill_rect(cv, rc, theme.line); }

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

void project_sftp_row_free(void *v) { RowData *d = v; free(d->name); free(d->size); free(d->when); free(d); }

int project_sftp_twisty_left(const RECT *rc, int depth) { return rc->left + px(6) + depth * px(INDENT); }

void project_sftp_paint_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    RowData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    if (d->selected) fill_round_rect(cv, rc, px(5), theme.raise, theme.line);
    else if (hovered) fill_round_rect(cv, rc, px(5), blend(theme.raise, theme.canvas, 0.5), blend(theme.raise, theme.canvas, 0.5));
    int x = project_sftp_twisty_left(rc, d->depth);
    RECT tw = { x, rc->top, x + px(16), rc->bottom };
    if (d->folder) draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &tw, FONT_ICON_SMALL, theme.muted);
    RECT icon = { tw.right + px(2), rc->top, tw.right + px(22), rc->bottom };
    wchar_t glyph = d->folder ? (d->home ? 0xE80F : d->open ? 0xE838 : 0xE8B7) : d->link ? 0xE71B : 0xE8A5;
    draw_glyph(cv, glyph, &icon, FONT_ICON_SMALL, d->folder ? theme.accent : theme.secondary);
    int right = rc->right - px(8);
    int when_w = px(120), size_w = px(80);
    if (d->when) {
        RECT wr = { right - when_w, rc->top, right, rc->bottom };
        draw_text(cv, d->when, &wr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    right -= when_w + px(12);
    if (d->size) {
        RECT sr = { right - size_w, rc->top, right, rc->bottom };
        draw_text(cv, d->size, &sr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    right -= size_w + px(12);
    RECT t = { icon.right + px(6), rc->top, right, rc->bottom };
    draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void project_sftp_paint_columns(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; (void)it;
    int right = rc->right - px(8);
    RECT wr = { right - px(120), rc->top, right, rc->bottom };
    draw_text(cv, "Modified", &wr, FONT_CAPTION2, theme.tertiary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    right -= px(120 + 12);
    RECT sr = { right - px(80), rc->top, right, rc->bottom };
    draw_text(cv, "Size", &sr, FONT_CAPTION2, theme.tertiary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT nr = { rc->left + px(6), rc->top, sr.left, rc->bottom };
    draw_text(cv, "Name", &nr, FONT_CAPTION2, theme.tertiary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/// The servers down the left, each with its name, user@host:port and its open sessions.
void project_sftp_layout_list(ProjectSftp *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading servers\xE2\x80\xA6"); return; }
    for (size_t i = 0; i < json_count(p->rows); i++) {
        const Json *row = json_at(p->rows, i);
        ServerRowData *d = xcalloc(1, sizeof *d);
        d->name = project_sftp_server_name(row);
        const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
        d->target = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", project_sftp_server_port(row));
        d->enabled = !json_bool_is(json_get(row, "enabled"), false);
        char *key = project_sftp_server_key(row);
        d->sessions = (int)sftp_count_for(key);
        for (size_t t = 0; t < sftp_count(); t++) if (str_eq(sftp_key(sftp_at(t)), key) && sftp_state(sftp_at(t)) != SFTP_CLOSED) d->live = true;
        free(key);
        doc_custom(doc, x, w, px(46), paint_server, d, server_row_free, p->base + A_SERVER, (intptr_t)i);
    }
    if (!json_count(p->rows) && !p->error)
        doc_text(doc, x + px(6), w - px(12), "No SSH servers for this project. Register one under \xE2\x9A\x99 Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

/// The project's sessions as tabs, wrapping, from `x` across `w`.
void project_sftp_layout_tabs(ProjectSftp *p, Doc *doc, int x0, int w) {
    int x = x0, y = doc->y, h = px(TAB_H), gap = px(4);
    SftpSession *shown = project_sftp_shown_session(p);
    for (size_t i = 0; i < sftp_count(); i++) {
        SftpSession *s = sftp_at(i);
        if (!str_eq(sftp_group(s), p->repo)) continue;
        TabData *d = xcalloc(1, sizeof *d);
        d->label = xstrdup(sftp_label(s)); d->active = s == shown; d->live = sftp_state(s) != SFTP_CLOSED;
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
