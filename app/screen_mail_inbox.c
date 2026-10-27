#include "screen_mail_inbox_internal.h"

static void body_done(void *owner, Request *r);
static void list_done(void *owner, Request *r);
static void accounts_done(void *owner, Request *r);
static void refresh(Screen *base);
static void timer(Screen *base, UINT id);
static void visible(Screen *base, bool shown);
static void activated(Screen *base, bool active);
static int footer_height(Screen *base, int width);
static void keycap(Canvas *cv, int *x, int y, int h, const char *screen_mail_inbox_key);
static void keylabel(Canvas *cv, int *x, int y, int h, int right, const char *label);
static void footer_paint(Screen *base, Canvas *cv, const RECT *rc);
static void destroy(Screen *base);

bool mail_inbox_offered(void) {
    return g_store.has_device && permission_rank(g_store.device.permission) == 2
        && store_supports("settings_mail_accounts") && store_supports("mail_messages");
}

bool mail_inbox_result_current(bool shown, int generation, const Request *r) {
    return shown && !r->cancelled && r->client && r->client == g_store.client
        && r->tag == generation && mail_inbox_offered() && store_supports(r->operation);
}

void screen_mail_inbox_text_set(char **slot, const char *text) { free(*slot); *slot = xstrdup(text); }

HWND screen_mail_inbox_window(Inbox *s) { return s->base.pane ? pane_hwnd(s->base.pane) : NULL; }

void screen_mail_inbox_repaint(Inbox *s) {
    if (s->base.pane) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
}

void screen_mail_inbox_cancel(Inbox *s) {
    request_cancel(&s->account_read); request_cancel(&s->list_read); request_cancel(&s->body_read);
    if (screen_mail_inbox_window(s)) KillTimer(screen_mail_inbox_window(s), TIMER_INBOX);
}

void screen_mail_inbox_clear_selection(Inbox *s) {
    screen_mail_reply_cancel(s); request_cancel(&s->body_read); mail_message_free(&s->body);
    screen_mail_inbox_text_set(&s->selected_id, NULL); screen_mail_inbox_text_set(&s->body_error, NULL); s->selected_account = 0;
    s->reveal_open = false; s->reveal_block = false; s->block_held = false; s->page_follow = 0;
}

void screen_mail_inbox_reset_list(Inbox *s) {
    request_cancel(&s->list_read); screen_mail_inbox_clear_selection(s); mail_messages_free(&s->messages);
    screen_mail_inbox_text_set(&s->list_error, NULL); screen_mail_inbox_text_set(&s->notice, NULL); screen_mail_inbox_text_set(&s->limit_note, NULL); s->loaded = false;
    // Old account responses must also be discarded across a filter generation.
    request_cancel(&s->account_read);
    s->focus_message = -1; s->menu = MENU_NONE;
    s->generation = s->generation == INT_MAX ? 1 : s->generation + 1;
}

void screen_mail_inbox_clear_private(Inbox *s) {
    request_cancel(&s->deletion); screen_mail_inbox_cancel(s); screen_mail_inbox_reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
    mail_filter_free(&s->filter); screen_mail_inbox_text_set(&s->account_error, NULL);
    api_client_release(s->mail_client); s->mail_client = NULL;
    s->kind = KIND_PEOPLE; s->group = GROUP_THREADS;
}

bool screen_mail_inbox_current(Inbox *s, Request *r) {
    if (mail_inbox_result_current(s->shown, s->generation, r)) return true;
    if (!mail_inbox_offered() || r->client != g_store.client) { screen_mail_inbox_clear_private(s); screen_mail_inbox_repaint(s); }
    return false;
}

bool screen_mail_inbox_ready(Inbox *s) {
    if (s->mail_client && s->mail_client != g_store.client) return false;
    return s->shown && !s->modal && !s->deletion && g_store.active && g_store.client && mail_inbox_offered() && GetTickCount64() >= g_store.mail_retry_until;
}

void screen_mail_inbox_arm(Inbox *s) {
    if (!s->shown || s->modal || !g_store.active || !mail_inbox_offered() || !screen_mail_inbox_window(s)) return;
    ULONGLONG tick = GetTickCount64(), delay = tick < g_store.mail_retry_until ? g_store.mail_retry_until - tick : 30000;
    SetTimer(screen_mail_inbox_window(s), TIMER_INBOX, (UINT)(delay > INT_MAX ? INT_MAX : delay), NULL);
}

void screen_mail_inbox_failure(Inbox *s, Request *r, char **error, bool detail) {
    screen_mail_inbox_text_set(error, mail_read_error(r->error.status, detail));
    if (r->error.status == 401 || r->error.status == 403
        || (r->error.status == 404 && str_eq(r->operation, "settings_mail_accounts"))) {
        screen_mail_inbox_clear_private(s); screen_mail_inbox_text_set(&s->account_error, mail_read_error(r->error.status, false)); return;
    }
    if (r->error.status == 409) {
        screen_mail_inbox_reset_list(s); mail_accounts_free(&s->accounts); s->accounts_loaded = false;
        screen_mail_inbox_text_set(&s->account_error, mail_read_error(409, false));
    }
    if (r->error.status == 429 || r->error.status == 503 || r->error.status == 0 || r->error.status >= 500) {
        if (g_store.mail_failures < 4) g_store.mail_failures++;
        ULONGLONG until = GetTickCount64() + (ULONGLONG)mail_settings_retry_ms(g_store.mail_failures, r->error.retry_after);
        if (until > g_store.mail_retry_until) g_store.mail_retry_until = until;
    }
    screen_mail_inbox_arm(s);
}

static void body_done(void *owner, Request *r) {
    Inbox *s = owner;
    if (!screen_mail_inbox_current(s, r) || !mail_account_readable(&s->accounts, s->selected_account)
        || json_int_or(json_get(r->args, "account"), 0) != s->selected_account
        || !str_eq(json_str(json_get(r->args, "id")), s->selected_id)) return;
    if (!r->ok) {
        if (r->error.status == 404) {
            screen_mail_inbox_reset_list(s); screen_mail_inbox_text_set(&s->notice, mail_read_error(404, true));
            screen_mail_inbox_load_accounts(s); // The next account snapshot validates removals before a fresh first page.
        } else screen_mail_inbox_failure(s, r, &s->body_error, true);
    } else {
        MailMessage fresh;
        if (!mail_message_parse(json_get(r->result, "message"), true, &fresh)) screen_mail_inbox_text_set(&s->body_error, "The server returned an unexpected message.");
        else {
            if (fresh.account_id != s->selected_account || !str_eq(fresh.id, s->selected_id)) {
                screen_mail_inbox_text_set(&s->body_error, "The server returned a different message; refresh the list."); mail_message_free(&fresh);
            } else { mail_message_free(&s->body); s->body = fresh; screen_mail_inbox_text_set(&s->body_error, NULL); g_store.mail_failures = 0; }
        }
    }
    screen_mail_inbox_repaint(s);
}

void screen_mail_inbox_load_body(Inbox *s) {
    if (!screen_mail_inbox_ready(s) || s->body_read || !s->selected_id || !store_supports("mail_message") || !mail_account_readable(&s->accounts, s->selected_account)) return;
    Json *args = json_object(); json_set_num(args, "account", s->selected_account); json_set_str(args, "id", s->selected_id);
    store_call("mail_message", args, 0, s, body_done, s->generation, &s->body_read); screen_mail_inbox_repaint(s);
}

static void list_done(void *owner, Request *r) {
    Inbox *s = owner; if (!screen_mail_inbox_current(s, r)) return;
    if (!r->ok) {
        screen_mail_inbox_failure(s, r, &s->list_error, false);
        if (r->error.status == 404) {
            screen_mail_inbox_clear_private(s); screen_mail_inbox_text_set(&s->account_error, mail_read_error(404, false));
        }
    } else {
        MailMessages page;
        if (!mail_messages_parse(r->result, &page)) screen_mail_inbox_text_set(&s->list_error, "The server returned an unexpected message list.");
        else {
            bool valid = true;
            for (size_t i = 0; i < page.count; i++) {
                if (!mail_account_find(&s->accounts, page.messages[i].account_id)
                    || (s->filter.account && page.messages[i].account_id != s->filter.account)) valid = false;
            }
            const char *cursor = json_str(json_get(r->args, "cursor"));
            if (page.next_cursor && str_eq(page.next_cursor, cursor)) valid = false;
            if (!valid) { mail_messages_free(&page); screen_mail_inbox_text_set(&s->list_error, "The page has unexpected accounts or a repeated cursor. Refresh account status and the list."); }
            else {
                // Revoked accounts may still occur in the server's synced copy; skip them without losing older pages.
                char *next = page.next_cursor; page.next_cursor = NULL;
                mail_messages_prune(&page, &s->accounts); page.next_cursor = next;
                mail_messages_append(&s->messages, &page); s->loaded = true; g_store.mail_failures = 0;
                screen_mail_inbox_text_set(&s->list_error, NULL);
            }
        }
    }
    screen_mail_inbox_repaint(s);
}

void screen_mail_inbox_load_list(Inbox *s) {
    if (!screen_mail_inbox_ready(s) || !s->accounts_loaded || s->list_read || (s->loaded && !s->messages.next_cursor)) return;
    if (s->filter.account && !mail_account_readable(&s->accounts, s->filter.account)) return;
    store_call("mail_messages", mail_filter_args(&s->filter, s->loaded ? s->messages.next_cursor : NULL), 0, s, list_done, s->generation, &s->list_read);
    screen_mail_inbox_repaint(s);
}

static void accounts_done(void *owner, Request *r) {
    Inbox *s = owner; if (!screen_mail_inbox_current(s, r)) return;
    if (!r->ok) screen_mail_inbox_failure(s, r, &s->account_error, false);
    else {
        MailAccounts fresh;
        if (!mail_accounts_parse(r->result, &fresh)) {
            screen_mail_inbox_clear_private(s); screen_mail_inbox_text_set(&s->account_error, "The server returned an unexpected account list. Retry account status.");
        } else {
            bool changed = false;
            for (size_t i = 0; i < s->accounts.count; i++) {
                const MailAccount *a = &s->accounts.accounts[i];
                if (mail_account_readable(&s->accounts, a->id) && !mail_account_readable(&fresh, a->id)) changed = true;
                if (!mail_reply_same_account(a, mail_account_find(&fresh, a->id))) changed = true;
            }
            for (size_t i = 0; i < fresh.count; i++) {
                const MailAccount *a = &fresh.accounts[i];
                if (mail_account_readable(&fresh, a->id) && !mail_account_readable(&s->accounts, a->id)) changed = true;
            }
            if (changed) screen_mail_inbox_reset_list(s); // Reload skipped rows as well as clearing removed/revoked accounts.
            mail_accounts_free(&s->accounts); s->accounts = fresh; s->accounts_loaded = true;
            // Account polling must not reset backoff while the first message page is still failing.
            if (s->loaded) g_store.mail_failures = 0;
            if (s->filter.account && !mail_account_readable(&fresh, s->filter.account)) { s->filter.account = 0; screen_mail_inbox_reset_list(s); }
            screen_mail_inbox_text_set(&s->account_error, NULL);
            if (!s->loaded) screen_mail_inbox_load_list(s);
        }
    }
    screen_mail_inbox_arm(s); screen_mail_inbox_repaint(s);
}

void screen_mail_inbox_load_accounts(Inbox *s) {
    if (s->mail_client != g_store.client) { screen_mail_inbox_clear_private(s); s->mail_client = api_client_retain(g_store.client); }
    if (!screen_mail_inbox_ready(s) || s->account_read) { screen_mail_inbox_arm(s); return; }
    store_call("settings_mail_accounts", json_object(), 0, s, accounts_done, s->generation, &s->account_read); screen_mail_inbox_repaint(s);
}

void screen_mail_inbox_reload(Inbox *s) { screen_mail_inbox_reset_list(s); screen_mail_inbox_load_accounts(s); screen_mail_inbox_repaint(s); }

void screen_mail_inbox_release(Inbox *s) { screen_mail_inbox_clear_private(s); screen_mail_inbox_text_set(&s->list_error, NULL); screen_mail_inbox_text_set(&s->body_error, NULL); screen_release(&s->base); }

void mail_inbox_set_thread_filter(Screen *base, const char *thread) {
    Inbox *s = (Inbox *)base;
    free(s->filter.thread);
    s->filter.thread = xstrdup(thread && *thread ? thread : NULL);
}

static void refresh(Screen *base) { Inbox *s = (Inbox *)base; if (screen_mail_inbox_ready(s)) screen_mail_inbox_reload(s); }

static void timer(Screen *base, UINT id) {
    if (id != TIMER_INBOX) return;
    Inbox *s = (Inbox *)base; if (screen_mail_inbox_window(s)) KillTimer(screen_mail_inbox_window(s), id);
    screen_mail_inbox_load_accounts(s); screen_mail_inbox_repaint(s);
}

static void visible(Screen *base, bool shown) {
    Inbox *s = (Inbox *)base; s->shown = shown;
    if (shown) screen_mail_inbox_load_accounts(s); else screen_mail_inbox_clear_private(s);
}

static void activated(Screen *base, bool active) {
    Inbox *s = (Inbox *)base;
    if (!active) screen_mail_inbox_cancel(s);
    else if (s->shown && !s->modal) { screen_mail_inbox_load_accounts(s); if (s->selected_id && !s->body.id) screen_mail_inbox_load_body(s); }
}

bool screen_mail_inbox_key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    Inbox *s = (Inbox *)base;
    if (ctrl || shift) return false;
    if (!s->shown || s->modal || s->deletion || !mail_inbox_offered()) return false;
    if (vk == VK_ESCAPE) {
        if (s->menu) { s->menu = MENU_NONE; screen_mail_inbox_repaint(s); return true; }
        if (s->selected_id) { screen_mail_inbox_clear_selection(s); screen_mail_inbox_repaint(s); return true; }
        return false;
    }
    if (vk == 'J' || vk == VK_DOWN) { screen_mail_inbox_move_focus(s, 1); return true; }
    if (vk == 'K' || vk == VK_UP) { screen_mail_inbox_move_focus(s, -1); return true; }
    if (vk == VK_RETURN) { if (s->focus_message >= 0) screen_mail_inbox_action(base, ACT_SELECT, s->focus_message, (POINT){0}); return true; }
    if (vk == 'M') { screen_mail_inbox_open_provider(s); return true; }
    if (vk == 'A') { screen_mail_inbox_cycle_account(s); return true; }
    if (vk == 'R') { if (store_supports("reply_mail_message")) screen_mail_reply_action(s, ACT_REPLY); else screen_mail_inbox_open_provider(s); return true; }
    if (vk == VK_DELETE) { screen_mail_inbox_action(base, store_supports("delete_mail_message") ? ACT_DELETE : ACT_DELETE_PROVIDER, 0, (POINT){0}); return true; }
    if (vk == VK_OEM_2) { screen_mail_inbox_edit_filter(s, ACT_Q); return true; }
    return false;
}

static int footer_height(Screen *base, int width) {
    (void)width; Inbox *s = (Inbox *)base;
    return s->shown && mail_inbox_offered() ? px(36) : 0;
}

static void keycap(Canvas *cv, int *x, int y, int h, const char *screen_mail_inbox_key) {
    int kw = text_width(cv, screen_mail_inbox_key, FONT_CAPTION2) + px(10);
    if (kw < px(18)) kw = px(18);
    RECT k = { *x, y, *x + kw, y + h };
    fill_round_rect(cv, &k, px(4), theme.field, theme.line);
    draw_text(cv, screen_mail_inbox_key, &k, FONT_CAPTION2, theme.ink, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    *x += kw + px(4);
}

static void keylabel(Canvas *cv, int *x, int y, int h, int right, const char *label) {
    int lw = text_width(cv, label, FONT_CAPTION2);
    if (*x + lw > right) return;
    RECT l = { *x, y, *x + lw, y + h };
    draw_text(cv, label, &l, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    *x += lw + px(12);
}

static void footer_paint(Screen *base, Canvas *cv, const RECT *rc) {
    (void)base;
    fill_rect(cv, rc, theme.canvas);
    draw_line(cv, rc->left, rc->top, rc->right, rc->top, theme.line);
    int h = px(18), y = rc->top + (rc->bottom - rc->top - h) / 2, x = rc->left + px(14);
    keycap(cv, &x, y, h, "j"); keycap(cv, &x, y, h, "k"); keylabel(cv, &x, y, h, rc->right, "move");
    keycap(cv, &x, y, h, "Enter"); keylabel(cv, &x, y, h, rc->right, "open");
    keycap(cv, &x, y, h, "Esc"); keylabel(cv, &x, y, h, rc->right, "close");
    keycap(cv, &x, y, h, "r"); keylabel(cv, &x, y, h, rc->right, "reply");
    keycap(cv, &x, y, h, "Del"); keylabel(cv, &x, y, h, rc->right, store_supports("delete_mail_message") ? "delete message" : "delete at provider");
    keycap(cv, &x, y, h, "m"); keylabel(cv, &x, y, h, rc->right, "open at provider");
    keycap(cv, &x, y, h, "a"); keylabel(cv, &x, y, h, rc->right, "switch mailbox");
}

static void destroy(Screen *base) {
    Inbox *s = (Inbox *)base; screen_mail_inbox_clear_private(s); s->shown = false;
    if (s->modal) { s->retired = true; return; }
    screen_mail_inbox_release(s);
}

static const ScreenVTable vt = { .destroy = destroy, .layout = screen_mail_inbox_layout, .header = screen_mail_inbox_header, .action = screen_mail_inbox_action,
    .refresh = refresh, .timer = timer, .visible = visible, .activated = activated, .key = screen_mail_inbox_key, .scrolled = screen_mail_inbox_scrolled,
    .footer_height = footer_height, .footer_paint = footer_paint };

Screen *mail_screen_new(void) {
    Inbox *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("mail");
    mail_filter_init(&s->filter); s->focus_message = -1; s->focus_y = s->open_y = s->reader_y = -1; return &s->base;
}
