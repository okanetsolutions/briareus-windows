#include "screen_settings_internal.h"

static void paint_ssh_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void ssh_field(SshForm *s, Doc *doc, int x, int w, int f);
static void paint_ssh_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void ssh_select(SshForm *s, Doc *doc, int x, int w, int p, const char *label, const char *hint);
static void ssh_cell(SshForm *s, Doc *doc, int x, int w, int cell);
static void ssh_row(SshForm *s, Doc *doc, int x, int col, int left, int right);

static const struct { const char *title; wchar_t glyph; } SSH_TABS[ST_COUNT] = {
    [ST_SERVER] = { "SSH server", 0xE968 }, [ST_DATABASE] = { "Database", 0xE1D3 },
};

// MARK: Layout

static void paint_ssh_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    SshForm *s = it->data;
    fill_round_rect(cv, rc, px(6), theme.raise, s->focused == (int)it->arg ? theme.accent_dim : theme.line);
}

/// A labelled box with its edit, the hint behind a help icon beside the label; advances.
static void ssh_field(SshForm *s, Doc *doc, int x, int w, int f) {
    const FieldDef *d = &screen_settings_SSH_FIELDS[f];
    doc_field_label(doc, x, w, d->label, theme.ink, d->hint);
    doc_space(doc, px(6));
    FontId fid = d->mono ? FONT_MONO : FONT_BODY;
    int fh = edit_line_height(fid), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_ssh_box));
    it->data = s; it->arg = f; it->action = ACT_SSH_FOCUS;
    s->rects[f] = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->laid[f] = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void paint_ssh_check(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    SshForm *s = it->data;
    bool hovered = doc_item_hovered(doc, it);
    int size = px(15), top = rc->top + (rc->bottom - rc->top - size) / 2;
    RECT b = { rc->left, top, rc->left + size, top + size };
    fill_round_rect(cv, &b, px(3), s->enabled ? theme.accent : theme.field, s->enabled ? theme.accent : hovered ? theme.accent_dim : theme.line_strong);
    if (s->enabled) draw_glyph(cv, 0xE73E, &b, FONT_ICON_SMALL, theme.on_accent);
    RECT t = { b.right + px(8), rc->top, rc->right, rc->bottom };
    draw_text(cv, "Available to sessions on this project", &t, FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/// A labelled picker, as the web's `<select>`; advances.
static void ssh_select(SshForm *s, Doc *doc, int x, int w, int p, const char *label, const char *hint) {
    doc_field_label(doc, x, w, label, theme.ink, hint);
    doc_space(doc, px(6));
    RECT box = { x, doc->y, x + w, doc->y + px(36) };
    Item *it = doc_item(doc, doc_add(doc, &box, screen_settings_paint_select));
    SelectData *d = xcalloc(1, sizeof *d);
    const char *v = s->picks[p];
    d->text = xstrdup(p == SP_MODE ? screen_settings_mode_title(v) : !str_empty(v) ? v : "Choose a project");
    d->enabled = true;
    it->data = d; it->free_data = screen_settings_select_free; it->action = ACT_SSH_PICK; it->arg = p; it->hand = true;
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

static void ssh_cell(SshForm *s, Doc *doc, int x, int w, int cell) {
    switch (cell) {
    case C_LABEL: ssh_field(s, doc, x, w, S_LABEL); break;
    case C_REPO: ssh_select(s, doc, x, w, SP_REPO, "Project", "Only this project's sessions see the server, through their SSH tool."); break;
    case C_HOST: ssh_field(s, doc, x, w, S_HOST); break;
    case C_PORT: ssh_field(s, doc, x, w, S_PORT); break;
    case C_USER: ssh_field(s, doc, x, w, S_USER); break;
    case C_KEY: ssh_field(s, doc, x, w, S_KEY); break;
    case C_ENABLED: {
        // Level with the boxes beside it: below where their label sits, centred on their height.
        doc_space(doc, font_height(doc->cv, FONT_FOOTNOTE) + px(6));
        int i = doc_custom(doc, x, w, px(36), paint_ssh_check, s, NULL, ACT_SSH_TOGGLE, 0);
        doc_item(doc, i)->hand = true;
        doc_space(doc, px(14));
        break;
    }
    case C_MODE: ssh_select(s, doc, x, w, SP_MODE, "Permission mode", "Ask shows the exact command in the dashboard for approval. Don't ask anything sends every command immediately."); break;
    case C_DBHOST: ssh_field(s, doc, x, w, S_DBHOST); break;
    case C_DBPORT: ssh_field(s, doc, x, w, S_DBPORT); break;
    case C_DBUSER: ssh_field(s, doc, x, w, S_DBUSER); break;
    case C_DBPASS: ssh_field(s, doc, x, w, S_DBPASS); break;
    }
}

/// Two cells side by side, each half the width, as the web's `.field-row`; the row is as tall as its taller cell.
static void ssh_row(SshForm *s, Doc *doc, int x, int col, int left, int right) {
    int gap = px(14), half = (col - gap) / 2, top = doc->y;
    ssh_cell(s, doc, x, half, left);
    int bottom = doc->y;
    doc->y = top;
    ssh_cell(s, doc, x + half + gap, col - half - gap, right);
    if (doc->y < bottom) doc->y = bottom;
}

void screen_settings_ssh_layout(Screen *base, Doc *doc) {
    SshForm *s = (SshForm *)base;
    memset(s->laid, 0, sizeof s->laid);
    int col = doc->width, x = 0;
    doc_space(doc, px(8));
    char *why = screen_settings_ssh_unavailable();
    if (why) { doc_space(doc, px(10)); doc_text(doc, x, col, why, FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK); free(why); doc_space(doc, px(12)); return; }
    // The tabs, drawn as the other settings forms draw theirs, each with the dot when something on it is not saved yet.
    int h = px(42), tx = x, ty = doc->y;
    s->tab_dot = false;
    for (int t = 0; t < ST_COUNT; t++) {
        bool changed = s->dirty && screen_settings_ssh_tab_changed(s, t);
        s->tab_dot |= changed;
        char *title = changed ? xstrfmt("%s \xE2\x80\xA2", SSH_TABS[t].title) : xstrdup(SSH_TABS[t].title);
        doc_tab(doc, &tx, &ty, x, x + col, h, SSH_TABS[t].glyph, title, NULL, t == screen_settings_g_ssh_tab, ACT_SSH_TAB, t);
        free(title);
    }
    doc->y = ty + h;
    doc_rule(doc, x, col);
    doc_space(doc, px(18));
    if (s->error) { doc_notice_box(doc, x, col, s->error); doc_space(doc, px(16)); }
    if (screen_settings_g_ssh_tab == ST_SERVER) {
        ssh_row(s, doc, x, col, C_LABEL, C_REPO);
        ssh_row(s, doc, x, col, C_HOST, C_PORT);
        ssh_row(s, doc, x, col, C_USER, C_KEY);
        ssh_row(s, doc, x, col, C_MODE, C_ENABLED);
        screen_settings_note(doc, x, col, "First verify the server's host key and add it to the Briareus account's known_hosts file. Unknown or changed host keys are refused. SSH configuration aliases and interactive commands are not supported.");
    } else if (!screen_settings_ssh_db_offered()) {
        doc_text(doc, x, col, "This Briareus server does not store database logins yet: its API has no GET /settings/ssh/servers/{id}/db-credentials. Update the server, then reconnect.", FONT_FOOTNOTE, theme.muted, DT_LEFT | DT_WORDBREAK);
    } else {
        // The server's database, reached through a tunnel over SSH to it.
        screen_settings_note(doc, x, col, "A MySQL login on this server, stored encrypted. Clients reach the database through an SSH tunnel to this server, at the host and port below as the server itself sees them.");
        doc_space(doc, px(8));
        if (s->login_error) { char *text = xstrfmt("The stored login could not be read: %s", s->login_error); doc_notice_box(doc, x, col, text); free(text); doc_space(doc, px(16)); }
        ssh_row(s, doc, x, col, C_DBHOST, C_DBPORT);
        ssh_row(s, doc, x, col, C_DBUSER, C_DBPASS);
        if (s->req_login) screen_settings_note(doc, x, col, "Reading the stored login\xE2\x80\xA6");
        else if (screen_settings_ssh_has_login(s)) screen_settings_note(doc, x, col, "A login is stored. Type a password only to change it.");
    }
    doc_space(doc, px(40));
}

void screen_settings_ssh_header(Screen *base, HeaderInfo *info) {
    SshForm *s = (SshForm *)base;
    const char *label = json_str_nonempty(json_get(s->row, "label")), *host = json_str_nonempty(json_get(s->row, "host"));
    const char *user = json_str_nonempty(json_get(s->row, "username")), *repo = json_str_nonempty(json_get(s->row, "repo"));
    if (s->id) {
        snprintf(info->title, sizeof info->title, "%s", label ? label : host ? host : "SSH server");
        char *port = screen_settings_ssh_field_text(s->row, S_PORT);
        snprintf(info->subtitle, sizeof info->subtitle, "%s%s%s:%s \xC2\xB7 %s", user ? user : "", user ? "@" : "", host ? host : "", port, repo ? repo : "");
        free(port);
    } else {
        snprintf(info->title, sizeof info->title, "New SSH server");
        snprintf(info->subtitle, sizeof info->subtitle, "A server a project's agents may run commands on, with approval.");
    }
    if (!store_supports("settings_ssh_servers")) return;
    bool busy = s->req_save || s->req_delete;
    HeaderButton *b = &info->buttons[info->button_count++];
    snprintf(b->label, sizeof b->label, "%s", s->req_save ? "Saving\xE2\x80\xA6" : "Save");
    b->glyph = 0xE74E; b->action = ACT_SSH_SAVE; b->prominent = true; b->tip = "Save this SSH server (Ctrl+S)";
    b->enabled = !busy && (s->dirty || !s->id) && store_supports(s->id ? "update_ssh_server" : "create_ssh_server");
    if (!s->id) return;
    HeaderButton *c = &info->buttons[info->button_count++];
    c->glyph = 0xE8C8; c->action = ACT_SSH_CLONE; c->enabled = !busy && store_supports("create_ssh_server"); c->tip = "Clone into a new SSH server";
    HeaderButton *d = &info->buttons[info->button_count++];
    d->glyph = 0xE74D; d->action = ACT_SSH_DELETE; d->destructive = true; d->enabled = !busy && store_supports("delete_ssh_server"); d->tip = "Delete this SSH server";
}
