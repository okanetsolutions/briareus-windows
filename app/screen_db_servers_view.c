#include "screen_db_servers_internal.h"

static void layout_tabs(DbForm *s, Doc *doc, int x, int w);
static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void field(DbForm *s, Doc *doc, int x, int w, int f);
static void field_pair(DbForm *s, Doc *doc, int x, int w, int left, int right, int right_w);
static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void note(Doc *doc, int x, int w, const char *text);
static void layout_test(DbForm *s, Doc *doc, int x, int w);
static void layout_pool(DbForm *s, Doc *doc, int x, int w);

static const struct { const char *title; wchar_t glyph; } TABS[T_COUNT] = {
    [T_SERVER] = { "Server", 0xE1D3 }, [T_POOL] = { "Pool", 0xE716 },
};

// MARK: - Layout

static void layout_tabs(DbForm *s, Doc *doc, int x, int w) {
    int h = px(42), tx = x, ty = doc->y;
    for (int t = 0; t < T_COUNT; t++) {
        bool changed = s->dirty && screen_db_servers_tab_changed(s, t);
        if (t == screen_db_servers_g_tab) s->tab_dot = changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", TABS[t].title) : xstrdup(TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + w, h, TABS[t].glyph, title, NULL, t == screen_db_servers_g_tab, ACT_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, w);
    doc_space(doc, px(18));
}

static void paint_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    DbForm *s = it->data;
    (void)doc;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

/// A labelled box with its edit at (x, cursor), the hint behind a help icon beside the label; advances.
static void field(DbForm *s, Doc *doc, int x, int w, int f) {
    if (!screen_db_servers_row_has(s, screen_db_servers_FIELDS[f].key)) return;
    doc_field_label(doc, x, w, screen_db_servers_FIELDS[f].label, theme.ink, screen_db_servers_FIELDS[f].hint);
    doc_space(doc, px(6));
    int fh = edit_line_height(screen_db_servers_FIELDS[f].mono ? FONT_MONO : FONT_BODY), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_box));
    it->data = s; it->arg = f; it->action = ACT_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

/// Two fields side by side, the second `right_w` wide (the rest when 0); advances past the taller.
static void field_pair(DbForm *s, Doc *doc, int x, int w, int left, int right, int right_w) {
    int gap = px(14), rw = right_w ? right_w : (w - gap) / 2, top = doc->y;
    field(s, doc, x, w - gap - rw, left);
    int bottom = doc->y;
    doc->y = top;
    field(s, doc, x + w - rw, rw, right);
    if (doc->y < bottom) doc->y = bottom;
}

static void paint_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    DbForm *s = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), s->enabled ? theme.accent : theme.field, s->enabled ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->enabled) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, "In the pool: sessions may claim this server", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void note(Doc *doc, int x, int w, const char *text) {
    doc_text(doc, x, w, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_WORDBREAK);
    doc_space(doc, px(10));
}

/// Test connection and what it last said beside it.
static void layout_test(DbForm *s, Doc *doc, int x, int w) {
    if (!store_supports("test_db_server")) return;
    int top = doc->y;
    int i = doc_button(doc, x, 0, s->req_test ? "Connecting\xE2\x80\xA6" : "Test connection", BUTTON_BORDERED, ACT_TEST, 0, !s->req_test);
    RECT b = doc_item(doc, i)->rc;
    int bottom = doc->y;
    if (s->test_text) {
        doc->y = top;
        COLORREF color = s->test_ok ? theme.ok : theme.danger;
        int tx = b.right + px(10);
        RECT r = { tx, b.top, x + w, b.bottom };
        // One line beside the button when it fits, wrapped under it when it does not.
        if (text_width(doc->cv, s->test_text, FONT_CAPTION) <= r.right - r.left) doc_text_at(doc, &r, s->test_text, FONT_CAPTION, color, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        else { doc->y = bottom + px(8); doc_text(doc, x, w, s->test_text, FONT_CAPTION, color, DT_LEFT | DT_WORDBREAK); bottom = doc->y; }
    }
    doc->y = bottom;
    doc_space(doc, px(14));
}

/// The pool as a whole: how many sessions it lets open at once, and the projects that claim from it.
static void layout_pool(DbForm *s, Doc *doc, int x, int w) {
    (void)s;
    note(doc, x, w, "One session holds a server at a time, so the pool is the session cap: as many sessions may be open at once as there are entries in it. The session's project decides which database name is used on it (created if missing); its setup commands are where migrations and seeding happen, on every session.");
    size_t n = settings_pool_capacity(), total = json_count(settings_db_server_rows());
    char *off = total > n ? xstrfmt(" %zu more %s listed but taken out of the pool.", total - n, total - n == 1 ? "is" : "are") : xstrdup("");
    char *count = n ? xstrfmt("%zu server%s in the pool, so %zu session%s with a database may be open at once.%s", n, n == 1 ? "" : "s", n, n == 1 ? "" : "s", off)
                    : xstrfmt("No server is in the pool yet.%s", off);
    doc_text(doc, x, w, count, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_WORDBREAK);
    free(count); free(off);
    doc_space(doc, px(18));
    doc_text(doc, x, w, "Projects that claim a server", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(8));
    const Json *projects = settings_project_rows();
    size_t claiming = 0;
    for (size_t i = 0; i < json_count(projects); i++) {
        const Json *p = json_at(projects, i);
        if (!json_bool_is(json_get(p, "dbPoolEnabled"), true)) continue;
        const char *repo = json_str_nonempty(json_get(p, "repo")), *label = json_str_nonempty(json_get(p, "label"));
        const char *db = json_str_nonempty(json_get(p, "dbPoolDatabase"));
        char *line = db ? xstrfmt("%s \xC2\xB7 database %s", label ? label : repo ? repo : "Project", db) : xstrdup(label ? label : repo ? repo : "Project");
        doc_label(doc, x, w, 0xE8B7, line, FONT_FOOTNOTE, theme.ink);
        free(line);
        doc_space(doc, px(4));
        claiming++;
    }
    if (!claiming) note(doc, x, w, "None yet. A project claims a server once \xE2\x80\x9CGive each session a database server of its own\xE2\x80\x9D is ticked on its Database tab.");
}

void screen_db_servers_form_layout(Screen *base, Doc *doc) {
    DbForm *s = (DbForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    if (!store_supports("settings_db_servers")) {
        doc_space(doc, px(10));
        doc_text(doc, x, col, "The database pool needs an Admin token on a server that offers it (GET /settings/db-servers).", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
        doc_space(doc, px(12));
        return;
    }
    layout_tabs(s, doc, x, col);
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    switch (screen_db_servers_g_tab) {
    case T_SERVER:
        if (screen_db_servers_row_has(s, screen_db_servers_ENABLED_KEY)) {
            doc_item(doc, doc_custom(doc, x, col, px(26), paint_check, s, NULL, ACT_TOGGLE, 0))->hand = true;
            doc_space(doc, px(10));
        }
        field(s, doc, x, col, F_LABEL);
        field_pair(s, doc, x, col, F_HOST, F_PORT, px(140));
        field_pair(s, doc, x, col, F_USERNAME, F_PASSWORD, 0);
        layout_test(s, doc, x, col);
        break;
    case T_POOL: layout_pool(s, doc, x, col); break;
    }
    doc_space(doc, px(40));
}

void screen_db_servers_form_header(Screen *base, HeaderInfo *info) {
    DbForm *s = (DbForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *host = json_str_nonempty(json_get(s->row, "host"));
    char *address = xstrfmt("%s:%d", host ? host : "", json_int_or(json_get(s->row, "port"), 0));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : address);
        snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 a session claims this server for itself while it runs", address);
    } else {
        snprintf(info->title, sizeof info->title, "New database server");
        snprintf(info->subtitle, sizeof info->subtitle, "A server sessions claim one at a time, each for a database of its own.");
    }
    free(address);
    if (!store_supports("settings_db_servers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this server (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_db_server" : "create_db_server");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_CLONE; c->enabled = !busy && store_supports("create_db_server"); c->tip = "Clone into a new server";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_db_server"); d->tip = "Remove this server from the pool";
}
