#include "screen_mail_internal.h"

static void account_card(MailScreen *s, Doc *doc, int x, int w, const MailAccount *a);
static bool label_differs(MailScreen *s);
static void label_changed(MailScreen *s);
static LRESULT CALLBACK label_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);
static void paint_label_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void label_field(MailScreen *s, Doc *doc, int x, int w);
static void layout_tab(MailScreen *s, Doc *doc, int w);
static int margin_of(Pane *pane);

void screen_mail_header(Screen *base, HeaderInfo *info) {
    MailScreen *s = (MailScreen *)base;
    const MailAccount *a = s->focus > 0 ? mail_account_find(&s->accounts, s->focus) : NULL;
    if (a) {
        const char *title = str_empty(a->label) ? a->email : a->label;
        snprintf(info->title, sizeof info->title, "%s", title ? title : "Mailbox");
        if (!str_empty(a->label) && a->email) snprintf(info->subtitle, sizeof info->subtitle, "%s", a->email);
        else snprintf(info->subtitle, sizeof info->subtitle, "Gmail and Outlook. Read-only until you enable mail actions.");
        if (store_supports("update_mail_account")) {
            HeaderButton *b = &info->buttons[info->button_count++];
            snprintf(b->label, sizeof b->label, "%s", s->label_write ? "Saving\xE2\x80\xA6" : "Save");
            b->glyph = 0xE74E; b->action = ACT_SAVE; b->prominent = true; b->tip = "Save this mailbox (Ctrl+S)";
            b->enabled = s->dirty && screen_mail_can_write(s, "update_mail_account");
        }
        return;
    }
    snprintf(info->title, sizeof info->title, "Mail accounts");
    snprintf(info->subtitle, sizeof info->subtitle, "Gmail and Outlook. Read-only until you enable mail actions.");
}

static void account_card(MailScreen *s, Doc *doc, int x, int w, const MailAccount *a) {
    int pad = px(14), box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(8));
    int ix = x + pad, iw = w - pad * 2;
    const char *title = str_empty(a->label) ? a->email : a->label;
    doc_text(doc, ix, iw, title ? title : "Mailbox", FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_WORDBREAK);
    if (!str_empty(a->label) && a->email) { doc_space(doc, px(2)); doc_text(doc, ix, iw, a->email, FONT_CAPTION, theme.muted, DT_WORDBREAK); }
    doc_space(doc, px(8));
    COLORREF status_color = str_eq(a->status, "connected") ? theme.ok : str_eq(a->status, "reauth") ? theme.warn : theme.muted;
    char *days = xstrfmt("%d day%s", a->sync_days, a->sync_days == 1 ? "" : "s");
    char *counts = xstrfmt("%d messages, %d unread", a->messages, a->unread);
    BadgeSpec badges[] = {
        { 0, a->provider ? a->provider : "mail", theme.accent, true },
        { 0, a->status ? a->status : "unknown", status_color, true },
        { 0, a->enabled ? "Enabled" : "Disabled", a->enabled ? theme.ok : theme.muted, true },
        { 0, days, theme.muted, true },
        { 0, counts, theme.muted, true },
    };
    doc_badges(doc, ix, iw, badges, sizeof badges / sizeof *badges, theme.raise);
    char *last = a->last_sync_at ? format_relative((time_t)(a->last_sync_at / 1000)) : xstrdup("never");
    char *sync = xstrfmt("Last sync %s%s", last, a->syncing ? "  ·  Syncing" : "");
    doc_space(doc, px(8));
    doc_text(doc, ix, iw, sync, FONT_CAPTION, theme.muted, DT_WORDBREAK);
    free(sync); free(last); free(days); free(counts);
    if (!str_empty(a->last_sync_error)) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, a->last_sync_error); }
    if (str_eq(a->status, "reauth")) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, "Provider access expired or was revoked. Sign in again with this mailbox."); }
    bool edit = screen_mail_can_write(s, "update_mail_account");
    ButtonSpec buttons[] = {
        { 0, a->enabled ? "Disable" : "Enable", BUTTON_BORDERED, ACT_ENABLED, a->id, edit },
        { 0, "Sync days", BUTTON_BORDERED, ACT_DAYS, a->id, edit },
        { 0, "Sign in again", BUTTON_BORDERED, ACT_REAUTH, a->id, screen_mail_can_write(s, "connect_mail_account") && mail_provider_available(&s->accounts, a->provider) },
        { 0, "Enable mail actions", BUTTON_BORDERED, ACT_MANAGE, a->id, screen_mail_can_write(s, "connect_mail_account") && mail_provider_available(&s->accounts, a->provider) },
        { 0, "Sync now", BUTTON_BORDERED, ACT_SYNC, a->id, screen_mail_can_write(s, "sync_mail_account") && !a->syncing && str_eq(a->status, "connected") },
        { 0, "Disconnect", BUTTON_DESTRUCTIVE, ACT_DELETE, a->id, screen_mail_can_write(s, "delete_mail_account") },
    };
    const char *ops[] = { "update_mail_account", "update_mail_account", "connect_mail_account", "connect_mail_account", "sync_mail_account", "delete_mail_account" };
    ButtonSpec offered[6]; size_t n = 0;
    for (size_t k = 0; k < sizeof buttons / sizeof *buttons; k++) if (store_supports(ops[k])) offered[n++] = buttons[k];
    if (n) { doc_space(doc, px(12)); doc_button_row(doc, ix, iw, offered, n); }
    if (store_supports("connect_mail_account")) {
        doc_space(doc, px(8));
        doc_text(doc, ix, iw, "Enable mail actions requests permission to send and move mail, including moving messages to trash. Approve access in your provider's sign-in window.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    }
    doc_box_end(doc, box, pad);
    doc_space(doc, px(12));
}

char *screen_mail_edit_text(HWND edit) {
    int n = GetWindowTextLengthW(edit);
    wchar_t *w = xmalloc(((size_t)n + 1) * sizeof *w);
    GetWindowTextW(edit, w, n + 1);
    char *text = wide_to_utf8(w); free(w);
    return text;
}

void screen_mail_set_edit_text(HWND edit, const char *text) { wchar_t *w = utf8_to_wide(text ? text : ""); SetWindowTextW(edit, w); free(w); }

const char *screen_mail_saved_label(const MailScreen *s) {
    const MailAccount *a = mail_account_find(&s->accounts, s->focus);
    return a && a->label ? a->label : "";
}

static bool label_differs(MailScreen *s) {
    if (!s->label_edit || s->focus <= 0) return false;
    char *now = screen_mail_edit_text(s->label_edit);
    bool differs = !str_eq(now, screen_mail_saved_label(s));
    free(now);
    return differs;
}

static void label_changed(MailScreen *s) {
    if (s->filling) return;
    bool dirty = label_differs(s);
    if (dirty == s->dirty) return;
    s->dirty = dirty;
    pane_header_changed(s->base.pane);
    pane_relayout(s->base.pane);
}

void screen_mail_label_fill(MailScreen *s) {
    if (!s->label_edit || s->dirty || s->focus <= 0 || !mail_account_find(&s->accounts, s->focus)) return;
    s->filling = true;
    screen_mail_set_edit_text(s->label_edit, screen_mail_saved_label(s));
    s->filling = false;
    s->dirty = false;
}

void screen_mail_label_ensure(MailScreen *s) {
    if (s->label_edit || !s->base.pane) return;
    HWND e = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10, pane_hwnd(s->base.pane), (HMENU)(INT_PTR)ID_MAIL_LABEL, GetModuleHandleW(NULL), NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)font(FONT_BODY), TRUE);
    SendMessageW(e, EM_SETLIMITTEXT, 200, 0); // The server refuses a label longer than 200 characters.
    { wchar_t *w = utf8_to_wide("shown in the mail menu"); SendMessageW(e, EM_SETCUEBANNER, TRUE, (LPARAM)w); free(w); }
    SetWindowSubclass(e, label_proc, ID_MAIL_LABEL, (DWORD_PTR)s);
    theme_apply_control(e);
    s->label_edit = e;
    screen_mail_label_fill(s);
}

void screen_mail_label_save(MailScreen *s) {
    if (!s->dirty || !s->label_edit || !screen_mail_can_write(s, "update_mail_account")) return;
    const MailAccount *a = mail_account_find(&s->accounts, s->focus);
    if (!a) return;
    char *now = screen_mail_edit_text(s->label_edit), *label = str_trim(now);
    free(now);
    if (str_eq(label, a->label ? a->label : "")) {
        s->filling = true; screen_mail_set_edit_text(s->label_edit, label); s->filling = false; s->dirty = false;
        free(label); pane_header_changed(s->base.pane); pane_relayout(s->base.pane); return;
    }
    char *days = xstrfmt("%d", a->sync_days);
    Json *body = mail_settings_body(label, a->enabled, days);
    free(label); free(days);
    if (!body) return;
    json_object_remove(body, "enabled");
    json_object_remove(body, "syncDays");
    json_set_num(body, "id", a->id);
    s->label_write = true;
    store_call("update_mail_account", body, 0, s, screen_mail_write_done, a->id, &s->write);
    screen_mail_repaint(s);
}

static LRESULT CALLBACK label_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    MailScreen *s = (MailScreen *)ref;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    switch (msg) {
    case WM_KEYDOWN:
        if (ctrl && wp == 'S') { screen_mail_label_save(s); return 0; }
        if (ctrl && wp == 'A') { SendMessageW(hwnd, EM_SETSEL, 0, -1); return 0; }
        if (wp == VK_ESCAPE) { SetFocus(GetParent(hwnd)); return 0; }
        if (wp == VK_RETURN) { screen_mail_label_save(s); return 0; }
        break;
    case WM_CHAR:
        if (wp == 0x13 || wp == 0x01 || wp == 0x1B || wp == '\r') return 0;
        break;
    case WM_MOUSEWHEEL: SendMessageW(GetParent(hwnd), msg, wp, lp); return 0;
    case WM_NCDESTROY: RemoveWindowSubclass(hwnd, label_proc, id); break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static void paint_label_box(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    MailScreen *s = it->data;
    bool on = s->label_edit && IsWindowEnabled(s->label_edit);
    COLORREF border = s->label_focused ? theme.accent_dim : theme.line;
    fill_round_rect(cv, rc, px(6), theme.raise, on ? border : blend(border, theme.canvas, 0.5));
}

/// The Label box, as a project form lays its fields out. The edit itself is placed over it.
static void label_field(MailScreen *s, Doc *doc, int x, int w) {
    screen_mail_label_ensure(s);
    bool on = store_supports("update_mail_account") && !s->blocked;
    doc_field_label(doc, x, w, "Label", on ? theme.ink : theme.muted, NULL);
    doc_space(doc, px(6));
    int fh = edit_line_height(FONT_BODY), h = px(36);
    RECT box = { x, doc->y, x + w, doc->y + h };
    Item *it = doc_item(doc, doc_add(doc, &box, paint_label_box));
    it->data = s; it->action = ACT_FOCUS;
    s->label_rect = (RECT){ x + px(10), box.top + (h - fh) / 2, x + w - px(10), box.top + (h - fh) / 2 + fh };
    s->label_laid = true;
    if (s->label_edit) EnableWindow(s->label_edit, on);
    doc->y = box.bottom;
    doc_space(doc, px(14));
}

/// The project settings tab bar, with the one Mailbox tab. A dot follows the title while the label is unsaved.
static void layout_tab(MailScreen *s, Doc *doc, int w) {
    int h = px(42), tx = 0, ty = doc->y;
    doc_tab(doc, &tx, &ty, 0, w, h, 0xE715, s->dirty ? "Mailbox \xE2\x80\xA2" : "Mailbox", NULL, true, 0, 0);
    doc->y = ty + h;
    doc_rule(doc, 0, w);
    doc_space(doc, px(18));
}

void screen_mail_layout(Screen *base, Doc *doc) {
    MailScreen *s = (MailScreen *)base;
    int col = doc->width, x = px(20), w = col - 2 * x;
    s->label_laid = false;
    doc_space(doc, px(8));
    if (!mail_settings_offered()) { doc_notice(doc, x, w, "Mail settings are unavailable: this server must advertise the mail account route and this device needs an Admin token."); return; }
    layout_tab(s, doc, col);
    if (s->error) { doc_notice_box(doc, x, w, s->error); doc_space(doc, px(12)); }
    if (s->notice) { doc_text(doc, x, w, s->notice, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); doc_space(doc, px(12)); }
    if (!s->loaded) { if (!s->blocked) doc_loading(doc, x, w, "Loading mail accounts..."); return; }
    if (s->sign_in.state) {
        int pad = px(14), box = doc_box_begin(doc, x, w, pad, theme.raise, theme.accent_dim, px(8));
        int ix = x + pad, iw = w - pad * 2;
        doc_text(doc, ix, iw, "Finish signing in", FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_SINGLELINE);
        doc_space(doc, px(10));
        ButtonSpec pending[] = {
            { 0, "Paste callback address", BUTTON_PROMINENT, ACT_FINISH, 0, !s->write && !s->sign_in.server_finish && GetTickCount64() >= s->retry_until && store_supports("finish_mail_account") },
            { 0, "Browser finished: refresh", BUTTON_BORDERED, ACT_BROWSER_DONE, 0, !s->write && s->sign_in.server_finish },
            { 0, "Cancel waiting", BUTTON_BORDERED, ACT_CANCEL, 0, !s->write },
        };
        doc_button_row(doc, ix, iw, pending, sizeof pending / sizeof *pending);
        doc_box_end(doc, box, pad);
        doc_space(doc, px(16));
    }
    if (s->focus > 0) {
        const MailAccount *a = mail_account_find(&s->accounts, s->focus);
        if (!a) { s->label_laid = false; doc_text(doc, x, w, "This mailbox is no longer connected.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); return; }
        label_field(s, doc, x, w);
        account_card(s, doc, x, w, a);
        return;
    }
    doc_text(doc, x, w, "Add a mailbox", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_SINGLELINE);
    doc_space(doc, px(4));
    doc_text(doc, x, w, "Sign-in opens in the browser. Tokens stay on the server.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(10));
    ButtonSpec gmail = { 0, "Connect Gmail", BUTTON_PROMINENT, ACT_CONNECT, 0, screen_mail_can_write(s, "connect_mail_account") };
    ButtonSpec outlook = { 0, "Connect Outlook", BUTTON_PROMINENT, ACT_CONNECT, 1, screen_mail_can_write(s, "connect_mail_account") };
    ButtonSpec connect[2]; size_t connect_count = 0;
    // Only configured providers appear; each operation also requires its deployed route.
    if (s->accounts.gmail && store_supports("connect_mail_account")) connect[connect_count++] = gmail;
    if (s->accounts.outlook && store_supports("connect_mail_account")) connect[connect_count++] = outlook;
    if (connect_count) doc_button_row(doc, x, w, connect, connect_count);
    if (!s->accounts.gmail && !s->accounts.outlook) doc_text(doc, x, w, "No mail providers are configured on this server.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    if (!s->accounts.count) { doc_space(doc, px(16)); doc_text(doc, x, w, "No connected mailboxes yet.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); }
}

static int margin_of(Pane *pane) { RECT rc; GetClientRect(pane_hwnd(pane), &rc); return (rc.right - rc.left - pane_content_width(pane)) / 2; }

void screen_mail_place(Screen *base, const RECT *content, int scroll_y) {
    MailScreen *s = (MailScreen *)base;
    HWND e = s->label_edit;
    if (!e) return;
    if (!s->shown || !s->label_laid) { ShowWindow(e, SW_HIDE); return; }
    int m = margin_of(base->pane);
    RECT r = { content->left + m + s->label_rect.left, content->top + s->label_rect.top - scroll_y, content->left + m + s->label_rect.right, content->top + s->label_rect.bottom - scroll_y };
    RECT screen_mail_visible;
    if (!IntersectRect(&screen_mail_visible, &r, content)) { ShowWindow(e, SW_HIDE); return; }
    MoveWindow(e, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    bool clipped = !EqualRect(&screen_mail_visible, &r);
    if (clipped) SetWindowRgn(e, CreateRectRgn(screen_mail_visible.left - r.left, screen_mail_visible.top - r.top, screen_mail_visible.right - r.left, screen_mail_visible.bottom - r.top), TRUE);
    else if (s->label_clipped) SetWindowRgn(e, NULL, TRUE);
    s->label_clipped = clipped;
    ShowWindow(e, SW_SHOWNA);
}

void screen_mail_command(Screen *base, int id, int code, HWND control) {
    (void)control;
    MailScreen *s = (MailScreen *)base;
    if (id != ID_MAIL_LABEL || !s->label_edit) return;
    switch (code) {
    case EN_CHANGE: label_changed(s); break;
    case EN_SETFOCUS: {
        s->label_focused = true;
        RECT content = pane_content_rect(base->pane);
        int top = s->label_rect.top - px(40), bottom = s->label_rect.bottom + px(16), y = pane_scroll_y(base->pane);
        if (top < y || bottom > y + (content.bottom - content.top)) pane_scroll_to(base->pane, top);
        pane_repaint(base->pane);
        break;
    }
    case EN_KILLFOCUS: s->label_focused = false; pane_repaint(base->pane); break;
    }
}

bool screen_mail_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { screen_mail_label_save((MailScreen *)base); return true; }
    return false;
}
