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
       ACT_RESET, ACT_MORE, ACT_SELECT, ACT_CLOSE, ACT_PROVIDER, ACT_SETTINGS, ACT_RETRY_LIST, ACT_RETRY_BODY, ACT_RETRY_ACCOUNTS };
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
    if (act == ACT_SETTINGS) { if (mail_settings_offered()) pane_push(base->pane, mail_settings_screen_new()); return; }
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
    snprintf(info->subtitle, sizeof info->subtitle, "Synced copy, newest first; reading here keeps provider read state");
}
static void message_heading(Doc *doc, int x, int w, const MailMessage *m, const MailAccounts *accounts) {
    doc_text(doc, x, w, str_empty(m->subject) ? "(No subject)" : m->subject, m->is_read ? FONT_BODY : FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK);
    const MailAccount *a = mail_account_find(accounts, m->account_id);
    char *date = m->received_at ? format_relative((time_t)(m->received_at / 1000)) : xstrdup("Unknown date");
    char *line = xstrfmt("%s | %s | %s%s%s", m->sender, a ? a->email : "", date, m->is_read ? "" : " | Unread", m->is_starred ? " | Starred" : "");
    doc_text(doc, x, w, line, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); free(date); free(line);
}
static void layout(Screen *base, Doc *doc) {
    Inbox *s = (Inbox *)base; int x = px(20), w = doc->width - x * 2;
    doc_space(doc, px(16));
    if (!mail_inbox_offered()) { clear_private(s); doc_notice(doc, x, w, "Mail needs an Admin token and the deployed account and message-list routes."); return; }
    if (!store_supports("mail_message")) clear_selection(s);
    bool available = ready(s);
    if (s->notice) doc_notice(doc, x, w, s->notice);
    if (g_store.mail_retry_until > GetTickCount64()) doc_notice(doc, x, w, "Mail cooldown is active. Retry buttons become available when it ends.");
    ButtonSpec controls[] = {
        {0, "Search", BUTTON_PLAIN, ACT_Q, 0, true}, {0, "Label/folder", BUTTON_PLAIN, ACT_LABEL, 0, true},
        {0, "Thread", BUTTON_PLAIN, ACT_THREAD, 0, true}, {0, "Reset filters", BUTTON_PLAIN, ACT_RESET, 0, true},
        {0, "Account settings", BUTTON_PLAIN, ACT_SETTINGS, 0, mail_settings_offered()}
    };
    doc_button_row(doc, x, w, controls, sizeof controls / sizeof *controls);
    char *filters = xstrfmt("Search: %s | Label: %s | Thread: %s", s->filter.q ? s->filter.q : "Any", s->filter.label ? s->filter.label : "Any", s->filter.thread ? s->filter.thread : "Any");
    doc_text(doc, x, w, filters, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); free(filters);
    const char *unread[] = { "Any read state", "Read", "Unread" }, *inbox[] = { "Any folder", "Outside inbox", "Inbox" }, *starred[] = { "Any star state", "Unstarred", "Starred" };
    doc_segments(doc, x, w, unread, 3, s->filter.unread + 1, ACT_UNREAD, -1, true);
    doc_segments(doc, x, w, inbox, 3, s->filter.inbox + 1, ACT_INBOX, -1, true);
    doc_segments(doc, x, w, starred, 3, s->filter.starred + 1, ACT_STARRED, -1, true);
    doc_button(doc, x, 0, s->filter.account ? "All mailboxes" : "All mailboxes (selected)", BUTTON_PLAIN, ACT_ACCOUNT, 0, true);
    for (size_t i = 0; i < s->accounts.count; i++) {
        const MailAccount *a = &s->accounts.accounts[i];
        char *title = xstrfmt("%s%s%s", str_empty(a->label) ? a->email : a->label, a->id == s->filter.account ? " (selected)" : "", mail_account_readable(&s->accounts, a->id) ? "" : " (sign-in required)");
        doc_button(doc, x, 0, title, BUTTON_PLAIN, ACT_ACCOUNT, a->id, mail_account_readable(&s->accounts, a->id)); free(title);
    }
    if (s->account_error) { doc_notice(doc, x, w, s->account_error); doc_button(doc, x, 0, "Retry account status", BUTTON_PLAIN, ACT_RETRY_ACCOUNTS, 0, available && !s->account_read); }
    if (!s->accounts_loaded) { if (s->account_read) doc_loading(doc, x, w, "Loading mail accounts..."); return; }
    if (s->selected_id) {
        doc_rule(doc, x, w);
        doc_button(doc, x, 0, "Close message", BUTTON_PLAIN, ACT_CLOSE, 0, true);
        if (s->body_error) doc_notice(doc, x, w, s->body_error);
        if (s->body_read) doc_loading(doc, x, w, "Loading message body...");
        else if (!s->body.id) doc_button(doc, x, 0, "Retry message", BUTTON_PLAIN, ACT_RETRY_BODY, 0, available && store_supports("mail_message"));
        if (s->body.id) {
            MailMessage *m = &s->body; message_heading(doc, x, w, m, &s->accounts);
            if (!str_empty(m->to)) doc_labeled(doc, x, w, "To", m->to, theme.muted);
            if (!str_empty(m->cc)) doc_labeled(doc, x, w, "Cc", m->cc, theme.muted);
            if (!str_empty(m->reply_to)) doc_labeled(doc, x, w, "Reply-To", m->reply_to, theme.muted);
            if (m->web_url) doc_button(doc, x, 0, "Open at provider", BUTTON_PLAIN, ACT_PROVIDER, 0, true);
            if (m->truncated) doc_notice(doc, x, w, "The server truncated this body. Open at the provider to read the complete message.");
            doc_space(doc, px(12));
            // Literal selectable text: no Markdown, HTML, scripts, remote images, WebView or link actions.
            doc_text(doc, x, w, str_empty(m->text) ? "No plain-text body is available in the synced copy." : m->text, FONT_BODY, theme.ink, DT_WORDBREAK);
            if (m->attachment_count) doc_section(doc, x, w, "Attachments (metadata only)");
            for (size_t i = 0; i < m->attachment_count; i++) {
                MailAttachment *a = &m->attachments[i]; char *line = xstrfmt("%s | %s | %.0f bytes", a->name ? a->name : "Unnamed", a->mime_type ? a->mime_type : "Unknown type", a->size);
                doc_text(doc, x, w, line, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); free(line);
            }
        }
        doc_rule(doc, x, w);
    }
    if (s->list_error) { doc_notice(doc, x, w, s->list_error); doc_button(doc, x, 0, "Retry list", BUTTON_PLAIN, ACT_RETRY_LIST, 0, available && !s->list_read); }
    if (s->list_read) doc_loading(doc, x, w, "Loading messages...");
    if (s->loaded && !s->messages.count) doc_text(doc, x, w, s->messages.next_cursor
        ? "No readable messages on the loaded pages. Load older messages to keep looking."
        : "No synced messages match these filters.", FONT_BODY, theme.muted, DT_WORDBREAK);
    for (size_t i = 0; i < s->messages.count; i++) {
        MailMessage *m = &s->messages.messages[i]; int box = doc_box_begin(doc, x, w, px(10), theme.surface, theme.border, px(6));
        message_heading(doc, x + px(10), w - px(20), m, &s->accounts);
        doc_text(doc, x + px(10), w - px(20), m->snippet, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_box_end(doc, box, px(10));
        if (store_supports("mail_message")) doc_box_action(doc, box, ACT_SELECT, (intptr_t)i);
        doc_space(doc, px(8));
    }
    if (s->messages.next_cursor) doc_button(doc, x, 0, "Load older messages", BUTTON_PLAIN, ACT_MORE, 0, available && !s->list_read);
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
