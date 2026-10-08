// Read-only global inbox; private content never enters DiskCache or the preview WebView.
#include "screens.h"
#include "dialogs.h"
#include "mail.h"
#include "mail_inbox.h"
#include "mail_settings.h"
#include "str.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ACT_ACCOUNT = 1900, ACT_Q, ACT_LABEL, ACT_THREAD, ACT_UNREAD, ACT_INBOX, ACT_STARRED,
       ACT_RESET, ACT_MORE, ACT_SELECT, ACT_CLOSE, ACT_PROVIDER, ACT_RETRY_LIST, ACT_RETRY_BODY, ACT_RETRY_ACCOUNTS };
enum { TIMER_INBOX = 1910 };
typedef struct {
    Screen base;
    MailAccounts accounts;
    MailMessages messages;
    MailMessage body;
    MailFilter filter;
    Request *account_read, *list_read, *body_read;
    char *selected_id, *account_error, *list_error, *body_error, *notice;
    int selected_account, generation;
    bool shown, accounts_loaded, loaded, modal, retired;
} Inbox;

bool mail_inbox_offered(void) {
    return g_store.has_device && permission_rank(g_store.device.permission) == 2
        && store_supports("settings_mail_accounts") && store_supports("mail_messages");
}
bool mail_inbox_result_current(bool shown, int generation, const Request *r) {
    return shown && !r->cancelled && r->client && r->client == g_store.client
        && r->tag == generation && mail_inbox_offered() && store_supports(r->operation);
}
static void text_set(char **slot, const char *text) { free(*slot); *slot = xstrdup(text); }
static HWND window(Inbox *s) { return s->base.pane ? pane_hwnd(s->base.pane) : NULL; }
static void repaint(Inbox *s) {
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}
static void cancel(Inbox *s) {
    request_cancel(&s->account_read); request_cancel(&s->list_read); request_cancel(&s->body_read);
    if (window(s)) KillTimer(window(s), TIMER_INBOX);
}
static void clear_selection(Inbox *s) {
    request_cancel(&s->body_read); mail_message_free(&s->body);
    text_set(&s->selected_id, NULL); text_set(&s->body_error, NULL); s->selected_account = 0;
}
static void reset_list(Inbox *s) {
    request_cancel(&s->list_read); clear_selection(s); mail_messages_free(&s->messages);
    text_set(&s->list_error, NULL); text_set(&s->notice, NULL); s->loaded = false;
    // Old account responses must also be discarded across a filter generation.
    request_cancel(&s->account_read);
    s->generation = s->generation == INT_MAX ? 1 : s->generation + 1;
}
static void clear_private(Inbox *s) {
    cancel(s); reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
    mail_filter_free(&s->filter); text_set(&s->account_error, NULL);
}
static bool current(Inbox *s, Request *r) {
    if (mail_inbox_result_current(s->shown, s->generation, r)) return true;
    if (!mail_inbox_offered() || r->client != g_store.client) { clear_private(s); repaint(s); }
    return false;
}
static bool ready(Inbox *s) { return s->shown && !s->modal && g_store.active && g_store.client && mail_inbox_offered() && GetTickCount64() >= g_store.mail_retry_until; }
static void arm(Inbox *s) {
    if (!s->shown || s->modal || !g_store.active || !mail_inbox_offered() || !window(s)) return;
    ULONGLONG tick = GetTickCount64(), delay = tick < g_store.mail_retry_until ? g_store.mail_retry_until - tick : 30000;
    SetTimer(window(s), TIMER_INBOX, (UINT)(delay > INT_MAX ? INT_MAX : delay), NULL);
}
static void load_accounts(Inbox *s);
static void load_list(Inbox *s);
static void load_body(Inbox *s);
static void failure(Inbox *s, Request *r, char **error, bool detail) {
    text_set(error, mail_read_error(r->error.status, detail));
    if (r->error.status == 401 || r->error.status == 403
        || (r->error.status == 404 && str_eq(r->operation, "settings_mail_accounts"))) {
        clear_private(s); text_set(&s->account_error, mail_read_error(r->error.status, false)); return;
    }
    if (r->error.status == 409) {
        reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
        text_set(&s->account_error, mail_read_error(409, false));
    }
    if (r->error.status == 429 || r->error.status == 503 || r->error.status == 0 || r->error.status >= 500) {
        if (g_store.mail_failures < 4) g_store.mail_failures++;
        ULONGLONG until = GetTickCount64() + (ULONGLONG)mail_settings_retry_ms(g_store.mail_failures, r->error.retry_after);
        if (until > g_store.mail_retry_until) g_store.mail_retry_until = until;
    }
    arm(s);
}
static void body_done(void *owner, Request *r) {
    Inbox *s = owner;
    if (!current(s, r) || !mail_account_readable(&s->accounts, s->selected_account)
        || json_int_or(json_get(r->args, "account"), 0) != s->selected_account
        || !str_eq(json_str(json_get(r->args, "id")), s->selected_id)) return;
    if (!r->ok) {
        if (r->error.status == 404) {
            reset_list(s); text_set(&s->notice, mail_read_error(404, true));
            load_accounts(s); // The next account snapshot validates removals before a fresh first page.
        } else failure(s, r, &s->body_error, true);
    } else {
        MailMessage fresh;
        if (!mail_message_parse(json_get(r->result, "message"), true, &fresh)) text_set(&s->body_error, "The server returned an unexpected message.");
        else {
            if (fresh.account_id != s->selected_account || !str_eq(fresh.id, s->selected_id)) {
                text_set(&s->body_error, "The server returned a different message; refresh the list."); mail_message_free(&fresh);
            } else { mail_message_free(&s->body); s->body = fresh; text_set(&s->body_error, NULL); g_store.mail_failures = 0; }
        }
    }
    repaint(s);
}
static void load_body(Inbox *s) {
    if (!ready(s) || s->body_read || !s->selected_id || !store_supports("mail_message") || !mail_account_readable(&s->accounts, s->selected_account)) return;
    Json *args = json_object(); json_set_num(args, "account", s->selected_account); json_set_str(args, "id", s->selected_id);
    store_call("mail_message", args, 0, s, body_done, s->generation, &s->body_read); repaint(s);
}
static void list_done(void *owner, Request *r) {
    Inbox *s = owner; if (!current(s, r)) return;
    if (!r->ok) {
        failure(s, r, &s->list_error, false);
        if (r->error.status == 404) {
            clear_private(s); text_set(&s->account_error, mail_read_error(404, false));
        }
    } else {
        MailMessages page;
        if (!mail_messages_parse(r->result, &page)) text_set(&s->list_error, "The server returned an unexpected message list.");
        else {
            bool valid = true;
            for (size_t i = 0; i < page.count; i++) {
                if (!mail_account_find(&s->accounts, page.messages[i].account_id)
                    || (s->filter.account && page.messages[i].account_id != s->filter.account)) valid = false;
            }
            const char *cursor = json_str(json_get(r->args, "cursor"));
            if (page.next_cursor && str_eq(page.next_cursor, cursor)) valid = false;
            if (!valid) { mail_messages_free(&page); text_set(&s->list_error, "The page has unexpected accounts or a repeated cursor. Refresh account status and the list."); }
            else {
                // Revoked accounts may still occur in the server's synced copy; skip them without losing older pages.
                char *next = page.next_cursor; page.next_cursor = NULL;
                mail_messages_prune(&page, &s->accounts); page.next_cursor = next;
                mail_messages_append(&s->messages, &page); s->loaded = true; g_store.mail_failures = 0;
                text_set(&s->list_error, NULL);
            }
        }
    }
    repaint(s);
}
static void load_list(Inbox *s) {
    if (!ready(s) || !s->accounts_loaded || s->list_read || (s->loaded && !s->messages.next_cursor)) return;
    if (s->filter.account && !mail_account_readable(&s->accounts, s->filter.account)) return;
    store_call("mail_messages", mail_filter_args(&s->filter, s->loaded ? s->messages.next_cursor : NULL), 0, s, list_done, s->generation, &s->list_read);
    repaint(s);
}
static void accounts_done(void *owner, Request *r) {
    Inbox *s = owner; if (!current(s, r)) return;
    if (!r->ok) failure(s, r, &s->account_error, false);
    else {
        MailAccounts fresh;
        if (!mail_accounts_parse(r->result, &fresh)) {
            clear_private(s); text_set(&s->account_error, "The server returned an unexpected account list. Retry account status.");
        } else {
            bool changed = false;
            for (size_t i = 0; i < s->accounts.count; i++) {
                const MailAccount *a = &s->accounts.accounts[i];
                if (mail_account_readable(&s->accounts, a->id) && !mail_account_readable(&fresh, a->id)) changed = true;
            }
            for (size_t i = 0; i < fresh.count; i++) {
                const MailAccount *a = &fresh.accounts[i];
                if (mail_account_readable(&fresh, a->id) && !mail_account_readable(&s->accounts, a->id)) changed = true;
            }
            if (changed) reset_list(s); // Reload skipped rows as well as clearing removed/revoked accounts.
            mail_accounts_free(&s->accounts); s->accounts = fresh; s->accounts_loaded = true;
            // Account polling must not reset backoff while the first message page is still failing.
            if (s->loaded) g_store.mail_failures = 0;
            if (s->filter.account && !mail_account_readable(&fresh, s->filter.account)) { s->filter.account = 0; reset_list(s); }
            text_set(&s->account_error, NULL);
            if (!s->loaded) load_list(s);
        }
    }
    arm(s); repaint(s);
}
static void load_accounts(Inbox *s) {
    if (!ready(s) || s->account_read) { arm(s); return; }
    store_call("settings_mail_accounts", json_object(), 0, s, accounts_done, s->generation, &s->account_read); repaint(s);
}
static void reload(Inbox *s) { reset_list(s); load_accounts(s); repaint(s); }
static void release(Inbox *s) { clear_private(s); text_set(&s->list_error, NULL); text_set(&s->body_error, NULL); screen_release(&s->base); }
static void edit_filter(Inbox *s, int act) {
    char **slot = act == ACT_Q ? &s->filter.q : act == ACT_LABEL ? &s->filter.label : &s->filter.thread;
    const char *label = act == ACT_Q ? "Search sender, subject or snippet (200 characters maximum)" : act == ACT_LABEL ? "Exact label or folder name" : "Exact thread/conversation ID";
    s->modal = true; cancel(s);
    char *value = dialog_text(window(s), "Mail filter", label, "Apply", *slot ? *slot : "");
    s->modal = false;
    if (s->retired) { free(value); release(s); return; }
    if (!s->shown || !mail_inbox_offered()) { free(value); clear_private(s); repaint(s); return; }
    if (value) { free(*slot); *slot = value; reload(s); }
    else { if (!s->accounts_loaded || !s->loaded) load_accounts(s); if (s->selected_id && !s->body.id) load_body(s); arm(s); }
}
static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt; Inbox *s = (Inbox *)base;
    if (!s->shown || s->modal || !mail_inbox_offered()) return;
    if (act == ACT_Q || act == ACT_LABEL || act == ACT_THREAD) { edit_filter(s, act); return; }
    if (act == ACT_RESET) { mail_filter_free(&s->filter); reload(s); return; }
    if (act == ACT_ACCOUNT) {
        if (arg && !mail_account_readable(&s->accounts, (int)arg)) return;
        s->filter.account = (int)arg; reload(s); return;
    }
    int *flag = act == ACT_UNREAD ? &s->filter.unread : act == ACT_INBOX ? &s->filter.inbox : act == ACT_STARRED ? &s->filter.starred : NULL;
    if (flag) { if (arg < -1 || arg > 1) return; *flag = (int)arg; reload(s); return; }
    if (act == ACT_CLOSE) { clear_selection(s); repaint(s); return; }
    if (act == ACT_PROVIDER) {
        if (s->body.id && mail_account_readable(&s->accounts, s->body.account_id) && mail_message_web_url_safe(s->body.web_url)) open_web_url(s->body.web_url);
        return;
    }
    if (act == ACT_MORE || act == ACT_RETRY_LIST) { if (!s->accounts_loaded) load_accounts(s); else load_list(s); return; }
    if (act == ACT_RETRY_ACCOUNTS) { load_accounts(s); return; }
    if (act == ACT_RETRY_BODY) { load_body(s); return; }
    if (act == ACT_SELECT && arg >= 0 && (size_t)arg < s->messages.count && store_supports("mail_message")) {
        const MailMessage *m = &s->messages.messages[arg];
        if (!mail_account_readable(&s->accounts, m->account_id)) return;
        clear_selection(s); s->selected_id = xstrdup(m->id); s->selected_account = m->account_id;
        load_body(s); repaint(s); if (base->pane) pane_scroll_to_top(base->pane);
    }
}
static void header(Screen *base, HeaderInfo *info) {
    (void)base; snprintf(info->title, sizeof info->title, "Mail");
    snprintf(info->subtitle, sizeof info->subtitle, "Newest first. Opening a message here leaves it unread at the provider.");
}
typedef struct { char *shown; bool on; } PillData;
static void pill_free(void *p) { PillData *d = p; free(d->shown); free(d); }
static void paint_pill(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    PillData *d = it->data;
    bool hovered = doc_item_hovered(doc, it);
    bool on = d && d->on;
    COLORREF fill = on ? theme.accent : hovered && it->action ? theme.raise : theme.field;
    COLORREF border = on ? theme.accent : hovered && it->action ? theme.accent_dim : theme.line;
    COLORREF ink = on ? theme.on_accent : it->action ? theme.ink : theme.muted;
    fill_round_rect(cv, rc, (rc->bottom - rc->top) / 2, fill, border);
    RECT t = { rc->left + px(10), rc->top, rc->right - px(10), rc->bottom };
    draw_text(cv, d && d->shown ? d->shown : "", &t, FONT_CAPTION, ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// `text` is the hit-test name; `shown` is what the pill draws. The item's text stays exact for the account address.
static void add_pill(Doc *doc, int *x, int *y, int left, int right, int h, const char *text, const char *shown, bool on, bool enabled, int action, intptr_t arg) {
    int tw = text_width(doc->cv, shown, FONT_CAPTION) + px(22), maxw = right - left;
    if (tw > maxw) tw = maxw;
    if (tw < px(36)) tw = px(36);
    if (*x > left && *x + tw > right) { *x = left; *y += h + px(6); }
    RECT rc = { *x, *y, *x + tw, *y + h };
    Item *it = doc_item(doc, doc_add(doc, &rc, paint_pill));
    PillData *d = xcalloc(1, sizeof *d); d->shown = xstrdup(shown ? shown : ""); d->on = on;
    it->data = d; it->free_data = pill_free; it->text = xstrdup(text ? text : "");
    if (enabled) { it->action = action; it->arg = arg; it->hand = true; }
    *x += tw + px(6);
}
static void layout_mailboxes(Inbox *s, Doc *doc, int x, int w) {
    int left = x, right = x + w, h = px(28), cx = left, y = doc->y;
    add_pill(doc, &cx, &y, left, right, h, s->filter.account ? "All mailboxes" : "All mailboxes (selected)", "All mailboxes", s->filter.account == 0, true, ACT_ACCOUNT, 0);
    for (size_t i = 0; i < s->accounts.count; i++) {
        const MailAccount *a = &s->accounts.accounts[i];
        const char *name = str_empty(a->label) ? (a->email ? a->email : "") : a->label;
        bool readable = mail_account_readable(&s->accounts, a->id);
        char *shown = readable ? xstrdup(name) : xstrfmt("%s · sign in", name ? name : "");
        add_pill(doc, &cx, &y, left, right, h, name, shown, a->id == s->filter.account, readable, ACT_ACCOUNT, a->id);
        free(shown);
    }
    doc->y = y + h;
}
/// One labelled segmented row. The segment item itself has no text and arg -1, which the filter tests address in order.
static void filter_row(Doc *doc, int x, int w, const char *label, const char *const *titles, int selected, int action) {
    int y = doc->y, h = font_height(doc->cv, FONT_CAPTION2) + px(4), label_w = px(52);
    RECT lr = { x, y, x + label_w, y + h };
    doc_text_at(doc, &lr, label, FONT_CAPTION_SEMIBOLD, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_segments(doc, x + label_w + px(8), w - label_w - px(8), titles, 3, selected, action, -1, true);
    doc_space(doc, px(6));
}
static bool row_open(const Inbox *s, const MailMessage *m) {
    return s->selected_id && s->selected_account == m->account_id && str_eq(s->selected_id, m->id);
}
static void message_meta(Doc *doc, int x, int w, const MailMessage *m, const MailAccounts *accounts) {
    const MailAccount *a = mail_account_find(accounts, m->account_id);
    char *date = m->received_at ? format_relative((time_t)(m->received_at / 1000)) : xstrdup("Unknown date");
    char *line = xstrfmt("%s  ·  %s  ·  %s%s%s", m->sender ? m->sender : "", a && a->email ? a->email : "", date, m->is_read ? "" : "  ·  Unread", m->is_starred ? "  ·  Starred" : "");
    doc_text(doc, x, w, line, FONT_CAPTION, theme.muted, DT_WORDBREAK | DT_END_ELLIPSIS); free(date); free(line);
}
typedef struct { char *name, *meta; } AttachRow;
static void attach_free(void *p) { AttachRow *d = p; free(d->name); free(d->meta); free(d); }
static void paint_attach(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc; AttachRow *d = it->data;
    RECT chip = *rc; chip.right = chip.left + px(36); chip.bottom = chip.top + px(36);
    if (chip.bottom > rc->bottom) chip.bottom = rc->bottom;
    fill_round_rect(cv, &chip, px(6), theme.field, theme.line);
    draw_glyph(cv, 0xE8A5, &chip, FONT_ICON_SMALL, theme.muted);
    int tx = chip.right + px(10);
    RECT n = { tx, rc->top, rc->right, rc->top + px(18) };
    draw_text(cv, d->name, &n, FONT_CAPTION_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT m = { tx, n.bottom, rc->right, rc->bottom };
    draw_text(cv, d->meta, &m, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
static void layout_attachment(Doc *doc, int x, int w, const MailAttachment *a) {
    const char *name = a->name ? a->name : "Unnamed", *type = a->mime_type ? a->mime_type : "Unknown type";
    AttachRow *d = xcalloc(1, sizeof *d);
    d->name = xstrdup(name); d->meta = xstrfmt("%s  ·  %.0f bytes", type, a->size);
    Item *it = doc_item(doc, doc_custom(doc, x, w, px(36), paint_attach, d, attach_free, 0, 0));
    it->text = xstrfmt("%s | %s | %.0f bytes", name, type, a->size);
}
static void layout_message(Inbox *s, Doc *doc, int x, int w, size_t index) {
    const MailMessage *m = &s->messages.messages[index];
    bool unread = !m->is_read, open = row_open(s, m);
    COLORREF fill = open ? blend(theme.accent, theme.canvas, theme.dark ? 0.22 : 0.10) : unread ? theme.raise : theme.surface;
    COLORREF border = open ? theme.accent : theme.line;
    int pad = px(12), box = doc_box_begin(doc, x, w, pad, fill, border, px(8));
    int ix = x + pad, iw = w - pad * 2;
    // The subject item has to follow the box immediately: selecting a row clicks the item before its subject.
    doc_text(doc, ix, iw, str_empty(m->subject) ? "(No subject)" : m->subject, unread || open ? FONT_BODY_SEMIBOLD : FONT_BODY, theme.ink, DT_WORDBREAK);
    doc_space(doc, px(2));
    message_meta(doc, ix, iw, m, &s->accounts);
    if (!str_empty(m->snippet)) { doc_space(doc, px(4)); doc_text(doc, ix, iw, m->snippet, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); }
    doc_box_end(doc, box, pad);
    if (store_supports("mail_message")) doc_box_action(doc, box, ACT_SELECT, (intptr_t)index);
    doc_space(doc, px(8));
}
static void layout_reader(Inbox *s, Doc *doc, int x, int w, bool available) {
    int pad = px(16), box = doc_box_begin(doc, x, w, pad, theme.raise, theme.line, px(10));
    int ix = x + pad, iw = w - pad * 2;
    doc_button(doc, ix, 0, "Close message", BUTTON_BORDERED, ACT_CLOSE, 0, true);
    doc_space(doc, px(8));
    if (s->body_error) doc_notice(doc, ix, iw, s->body_error);
    if (s->body_read) doc_loading(doc, ix, iw, "Loading message body...");
    else if (!s->body.id) doc_button(doc, ix, 0, "Retry message", BUTTON_BORDERED, ACT_RETRY_BODY, 0, available && store_supports("mail_message"));
    if (s->body.id) {
        const MailMessage *m = &s->body;
        doc_text(doc, ix, iw, str_empty(m->subject) ? "(No subject)" : m->subject, FONT_TITLE, theme.ink, DT_WORDBREAK);
        doc_space(doc, px(4));
        message_meta(doc, ix, iw, m, &s->accounts);
        doc_space(doc, px(10));
        if (!str_empty(m->to)) doc_labeled(doc, ix, iw, "To", m->to, theme.ink);
        if (!str_empty(m->cc)) doc_labeled(doc, ix, iw, "Cc", m->cc, theme.ink);
        if (!str_empty(m->reply_to)) doc_labeled(doc, ix, iw, "Reply-To", m->reply_to, theme.ink);
        if (m->web_url || m->truncated) doc_space(doc, px(8));
        if (m->web_url) doc_button(doc, ix, 0, "Open at provider", BUTTON_BORDERED, ACT_PROVIDER, 0, true);
        if (m->truncated) { doc_space(doc, px(8)); doc_notice(doc, ix, iw, "The server truncated this body. Open at the provider to read the complete message."); }
        doc_space(doc, px(12));
        doc_rule(doc, ix, iw);
        doc_space(doc, px(12));
        // Literal selectable text: no Markdown, HTML, scripts, remote images, WebView or link actions.
        doc_text(doc, ix, iw, str_empty(m->text) ? "No plain-text body is available in the synced copy." : m->text, FONT_BODY, theme.ink, DT_WORDBREAK);
        if (m->attachment_count) { doc_space(doc, px(16)); doc_section(doc, ix, iw, "Attachments"); doc_space(doc, px(6)); }
        for (size_t i = 0; i < m->attachment_count; i++) { layout_attachment(doc, ix, iw, &m->attachments[i]); doc_space(doc, px(6)); }
    }
    doc_box_end(doc, box, pad);
    doc_space(doc, px(18));
}
static void layout(Screen *base, Doc *doc) {
    Inbox *s = (Inbox *)base; int x = px(20), w = doc->width - x * 2;
    doc_space(doc, px(16));
    if (!mail_inbox_offered()) { clear_private(s); doc_notice(doc, x, w, "Mail needs an Admin token and the deployed account and message-list routes."); return; }
    if (!store_supports("mail_message")) clear_selection(s);
    bool available = ready(s);
    if (s->notice) { doc_notice(doc, x, w, s->notice); doc_space(doc, px(8)); }
    if (g_store.mail_retry_until > GetTickCount64()) { doc_notice(doc, x, w, "Mail cooldown is active. Retry buttons become available when it ends."); doc_space(doc, px(8)); }
    ButtonSpec controls[] = {
        {0, "Search", BUTTON_BORDERED, ACT_Q, 0, true}, {0, "Label/folder", BUTTON_BORDERED, ACT_LABEL, 0, true},
        {0, "Thread", BUTTON_BORDERED, ACT_THREAD, 0, true}, {0, "Reset filters", BUTTON_BORDERED, ACT_RESET, 0, true}
    };
    doc_button_row(doc, x, w, controls, sizeof controls / sizeof *controls);
    if (s->filter.q || s->filter.label || s->filter.thread) {
        char *filters = xstrfmt("%s%s%s%s%s%s",
            s->filter.q ? "Search  " : "", s->filter.q ? s->filter.q : "",
            s->filter.label ? (s->filter.q ? "   ·   Label  " : "Label  ") : "", s->filter.label ? s->filter.label : "",
            s->filter.thread ? ((s->filter.q || s->filter.label) ? "   ·   Thread  " : "Thread  ") : "", s->filter.thread ? s->filter.thread : "");
        doc_space(doc, px(6));
        doc_text(doc, x, w, filters, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(filters);
    }
    doc_space(doc, px(14));
    layout_mailboxes(s, doc, x, w);
    doc_space(doc, px(12));
    static const char *const unread[] = { "Any", "Read", "Unread" }, *const inbox[] = { "Any", "Outside", "Inbox" }, *const starred[] = { "Any", "No", "Starred" };
    filter_row(doc, x, w, "Read", unread, s->filter.unread + 1, ACT_UNREAD);
    filter_row(doc, x, w, "Folder", inbox, s->filter.inbox + 1, ACT_INBOX);
    filter_row(doc, x, w, "Star", starred, s->filter.starred + 1, ACT_STARRED);
    if (s->account_error) { doc_space(doc, px(8)); doc_notice(doc, x, w, s->account_error); doc_button(doc, x, 0, "Retry account status", BUTTON_BORDERED, ACT_RETRY_ACCOUNTS, 0, available && !s->account_read); }
    if (!s->accounts_loaded) { if (s->account_read) doc_loading(doc, x, w, "Loading mail accounts..."); return; }
    if (s->accounts_loaded && !s->accounts.count && !s->account_error) {
        doc_space(doc, px(8));
        doc_text(doc, x, w, "No mailbox is connected. Add Gmail or Outlook in Settings, under Mail.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    }
    doc_space(doc, px(8));
    if (s->selected_id) layout_reader(s, doc, x, w, available);
    if (s->list_error) { doc_notice(doc, x, w, s->list_error); doc_button(doc, x, 0, "Retry list", BUTTON_BORDERED, ACT_RETRY_LIST, 0, available && !s->list_read); }
    if (s->list_read) doc_loading(doc, x, w, "Loading messages...");
    if (s->loaded && !s->messages.count) {
        doc_space(doc, px(8));
        doc_text(doc, x, w, s->messages.next_cursor
            ? "No readable messages on the loaded pages. Load older messages to keep looking."
            : "No synced messages match these filters.", FONT_BODY, theme.muted, DT_WORDBREAK);
    }
    for (size_t i = 0; i < s->messages.count; i++) layout_message(s, doc, x, w, i);
    if (s->messages.next_cursor) doc_button(doc, x, 0, "Load older messages", BUTTON_BORDERED, ACT_MORE, 0, available && !s->list_read);
}
static void refresh(Screen *base) { Inbox *s = (Inbox *)base; if (ready(s)) reload(s); }
static void timer(Screen *base, UINT id) {
    if (id != TIMER_INBOX) return;
    Inbox *s = (Inbox *)base; if (window(s)) KillTimer(window(s), id);
    load_accounts(s); repaint(s);
}
static void visible(Screen *base, bool shown) {
    Inbox *s = (Inbox *)base; s->shown = shown;
    if (shown) load_accounts(s); else clear_private(s);
}
static void activated(Screen *base, bool active) {
    Inbox *s = (Inbox *)base;
    if (!active) cancel(s);
    else if (s->shown && !s->modal) { load_accounts(s); if (s->selected_id && !s->body.id) load_body(s); }
}
static void destroy(Screen *base) {
    Inbox *s = (Inbox *)base; clear_private(s); s->shown = false;
    if (s->modal) { s->retired = true; return; }
    release(s);
}
static const ScreenVTable vt = { .destroy = destroy, .layout = layout, .header = header, .action = action,
    .refresh = refresh, .timer = timer, .visible = visible, .activated = activated };
Screen *mail_screen_new(void) {
    Inbox *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("mail"); mail_filter_init(&s->filter); return &s->base;
}
