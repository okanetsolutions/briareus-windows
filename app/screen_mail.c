// Global mailbox settings. Message lists and bodies belong to issue #124.
#include "screens.h"
#include "dialogs.h"
#include "mail.h"
#include "mail_settings.h"
#include <limits.h>
#include <math.h>
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { ACT_CONNECT = 1800, ACT_REAUTH, ACT_LABEL, ACT_ENABLED, ACT_DAYS, ACT_DELETE, ACT_SYNC, ACT_FINISH, ACT_CANCEL, ACT_BROWSER_DONE };
enum { TIMER_MAIL = 1810 };
typedef struct {
    Screen base;
    MailAccounts accounts;
    MailSignIn sign_in;
    Request *read, *write;
    char *error, *notice, *starting_provider;
    int starting_id, failures;
    bool shown, loaded, blocked, modal, retired, read_error;
    ULONGLONG next_read, retry_until;
} MailScreen;

bool mail_settings_offered(void) { return g_store.has_device && permission_rank(g_store.device.permission) == 2 && store_supports("settings_mail_accounts"); }
static double now_ms(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER n; n.LowPart = ft.dwLowDateTime; n.HighPart = ft.dwHighDateTime; return (double)(n.QuadPart / 10000ULL - 11644473600000ULL); }
static void set_text(char **slot, const char *text) { free(*slot); *slot = xstrdup(text); }
bool mail_settings_result_current(bool shown, const Request *r) { return shown && !r->cancelled && r->client && r->client == g_store.client && mail_settings_offered(); }
int mail_settings_retry_ms(int failures, double retry_after) {
    int delay = 10000;
    for (int i = 1; i < failures && delay < 60000; i++) delay = delay > 30000 ? 60000 : delay * 2;
    if (isfinite(retry_after) && retry_after > (double)delay / 1000) {
        delay = retry_after >= (double)INT_MAX / 1000 ? INT_MAX : (int)ceil(retry_after * 1000);
    }
    return delay;
}
static void set_error(MailScreen *s, const char *text, bool from_read) {
    set_text(&s->error, text); s->read_error = from_read;
}
static bool current(MailScreen *s, Request *r) { return mail_settings_result_current(s->shown, r); }
static bool can_write(MailScreen *s, const char *op) { return s->shown && !s->blocked && !s->write && !s->sign_in.state && !s->read && GetTickCount64() >= s->retry_until && store_supports(op); }
static void repaint(MailScreen *s) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }
static void load(MailScreen *s);
static void release(MailScreen *s) {
    mail_sign_in_free(&s->sign_in); mail_accounts_free(&s->accounts);
    free(s->error); free(s->notice); free(s->starting_provider); screen_release(&s->base);
}
static void arm(MailScreen *s, int delay) {
    if (!s->shown || s->blocked || s->modal || !g_store.active || !mail_settings_offered()) return;
    s->next_read = GetTickCount64() + (ULONGLONG)delay;
    SetTimer(pane_hwnd(s->base.pane), TIMER_MAIL, (UINT)delay, NULL);
}
static bool syncing(MailScreen *s) { for (size_t i = 0; i < s->accounts.count; i++) if (s->accounts.accounts[i].syncing) return true; return false; }
static void stop_waiting(MailScreen *s) { mail_sign_in_free(&s->sign_in); }
static void failed(MailScreen *s, Request *r, bool finish) {
    set_error(s, mail_error_message(r->error.status, finish, r->error.message), str_eq(r->operation, "settings_mail_accounts"));
    if (r->error.status == 401 || r->error.status == 403 || r->error.status == 404) s->blocked = true;
    if (r->error.status == 429) {
        s->failures++;
        int delay = mail_settings_retry_ms(s->failures, r->error.retry_after);
        s->retry_until = GetTickCount64() + (ULONGLONG)delay;
        arm(s, delay);
    }
}
static void read_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!current(s, r)) return;
    MailAccounts fresh;
    if (!r->ok) {
        failed(s, r, false);
        if (!s->blocked && r->error.status != 429) {
            s->failures++;
            int delay = mail_settings_retry_ms(s->failures, r->error.retry_after);
            s->retry_until = GetTickCount64() + (ULONGLONG)delay;
            arm(s, delay);
        }
    } else if (!mail_accounts_parse(r->result, &fresh)) {
        set_error(s, "The server returned an unexpected mail account list.", true);
        s->blocked = true;
    } else {
        if (s->read_error) set_error(s, NULL, false);
        s->loaded = true; s->failures = 0; s->next_read = 0; s->retry_until = 0;
        if (s->sign_in.state && mail_sign_in_completed(&s->sign_in, &fresh)) {
            stop_waiting(s); set_text(&s->notice, "The account list now reflects a connected mailbox.");
        }
        mail_accounts_free(&s->accounts); s->accounts = fresh;
        if (s->sign_in.state || syncing(s)) arm(s, 10000);
    }
    repaint(s);
}
static void load(MailScreen *s) {
    if (!s->shown || s->blocked || s->modal || s->read || s->write || !g_store.active || !mail_settings_offered()) return;
    ULONGLONG tick = GetTickCount64();
    if (tick < s->retry_until) { arm(s, (int)(s->retry_until - tick)); return; }
    if (tick < s->next_read) { arm(s, (int)(s->next_read - tick)); return; }
    store_call("settings_mail_accounts", json_object(), 0, s, read_done, 0, &s->read);
    repaint(s);
}
static void start_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!current(s, r)) return;
    if (!r->ok) failed(s, r, false);
    else if (!mail_sign_in_parse(r->result, s->starting_provider, s->starting_id, &s->accounts, &s->sign_in)
             || now_ms() >= s->sign_in.expires_at
             || (!s->sign_in.server_finish && !store_supports("finish_mail_account"))) {
        stop_waiting(s); set_error(s, "The server returned an invalid or expired sign-in. Start again.", false);
    } else {
        set_error(s, NULL, false);
        set_text(&s->notice, s->sign_in.server_finish
            ? "Finish sign-in in the browser. This page checks every 10 seconds. The browser reports errors, including a different mailbox (409). For an already connected mailbox, confirm the browser result then refresh here."
            : "Finish sign-in in the browser, then immediately paste the complete final callback address here; Microsoft codes may expire within a minute.");
        open_web_url(json_str(json_get(r->result, "url")));
        arm(s, 10000);
    }
    repaint(s);
}
static void start(MailScreen *s, const char *provider, int id) {
    if (!can_write(s, "connect_mail_account") || !s->loaded || !mail_provider_available(&s->accounts, provider)) return;
    Json *body = json_object(); json_set_str(body, "provider", provider);
    if (id) json_set_num(body, "accountId", id);
    else {
        json_set_str(body, "label", s->accounts.default_label ? s->accounts.default_label : "");
        json_set_bool(body, "enabled", s->accounts.default_enabled); json_set_num(body, "syncDays", s->accounts.default_sync_days);
    }
    set_text(&s->starting_provider, provider); s->starting_id = id;
    set_error(s, NULL, false); set_text(&s->notice, NULL);
    store_call("connect_mail_account", body, 0, s, start_done, 0, &s->write); repaint(s);
}
static void write_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!current(s, r)) return;
    bool finish = str_eq(r->operation, "finish_mail_account");
    if (finish) stop_waiting(s); // Exchanges are single-use, including a refusal or ambiguous network failure.
    if (!r->ok) { failed(s, r, finish); }
    else {
        bool deletion = str_eq(r->operation, "delete_mail_account");
        MailAccount a = {0};
        bool valid = deletion ? json_bool_is(json_get(r->result, "ok"), true) : mail_account_parse(json_get(r->result, "account"), &a);
        if (valid && !deletion) {
            if (!finish && a.id != r->tag) valid = false;
            if (finish && s->starting_id && a.id != s->starting_id) valid = false;
            mail_account_free(&a);
        }
        if (!valid) set_error(s, "The server returned an unexpected mail response. Refresh before trying again.", false);
        else {
            set_error(s, NULL, false);
            set_text(&s->notice, str_eq(r->operation, "sync_mail_account")
                ? "Sync started (202); polling account status until it finishes."
                : deletion ? "Disconnected. The provider may still list this app as having access; remove it there to revoke access."
                : "Account settings saved; refreshing status.");
        }
    }
    if (!s->blocked && GetTickCount64() >= s->retry_until) { s->next_read = 0; load(s); }
    repaint(s);
}
// A token revocation can replace the screen inside a modal dialog's nested message loop.
// Retain the screen until the dialog returns, then discard its input if it was retired.
static void modal_begin(MailScreen *s) {
    s->modal = true; KillTimer(pane_hwnd(s->base.pane), TIMER_MAIL); request_cancel(&s->read); s->next_read = 0;
}
static bool modal_end(MailScreen *s) {
    s->modal = false;
    if (s->retired) { release(s); return false; }
    if (s->sign_in.state || syncing(s)) arm(s, 10000);
    return true;
}
static void finish(MailScreen *s) {
    if (!s->sign_in.state || s->sign_in.server_finish || s->write || GetTickCount64() < s->retry_until || !store_supports("finish_mail_account")) return;
    modal_begin(s);
    char *url = dialog_text(pane_hwnd(s->base.pane), "Finish mail sign-in", "Paste the complete callback address immediately after sign-in", "Finish", "");
    if (!modal_end(s)) { free(url); return; }
    if (!url) return;
    if (!s->shown || s->blocked || GetTickCount64() < s->retry_until || !store_supports("finish_mail_account")) { free(url); return; }
    Json *body = mail_sign_in_finish(&s->sign_in, url, now_ms()); free(url);
    if (!body) { set_error(s, "Callback does not match this sign-in, contains an error, or has expired. Check the final address or start again.", false); repaint(s); return; }
    request_cancel(&s->read); KillTimer(pane_hwnd(s->base.pane), TIMER_MAIL);
    s->next_read = 0;
    store_call("finish_mail_account", body, 0, s, write_done, s->starting_id, &s->write); repaint(s);
}
static void update(MailScreen *s, const MailAccount *a, int action) {
    if (!can_write(s, "update_mail_account")) return;
    // Dialogs run a nested message loop: copy the id and values before status polling can replace the list.
    int id = a->id; bool enabled = a->enabled;
    char *label = xstrdup(a->label ? a->label : ""), *days = xstrfmt("%d", a->sync_days);
    Json *body = NULL;
    if (action == ACT_LABEL) {
        modal_begin(s);
        char *v = dialog_text(pane_hwnd(s->base.pane), "Mailbox label", "Label", "Save", label);
        if (!modal_end(s)) { free(v); free(label); free(days); return; }
        if (v) { body = mail_settings_body(v, enabled, days); free(v); }
    } else if (action == ACT_DAYS) {
        modal_begin(s);
        char *v = dialog_text(pane_hwnd(s->base.pane), "Mail sync window", "Days to keep (1-365); changing this restarts the first sync", "Save", days);
        if (!modal_end(s)) { free(v); free(label); free(days); return; }
        if (v) { body = mail_settings_body(label, enabled, v); if (!body) set_error(s, "Enter a whole number of days from 1 to 365.", false); free(v); }
    } else body = mail_settings_body(label, !enabled, days);
    free(label); free(days);
    if (body && can_write(s, "update_mail_account") && mail_account_find(&s->accounts, id)) {
        // Send only the field edited; another client may have changed the other settings during the dialog.
        if (action != ACT_LABEL) json_object_remove(body, "label");
        if (action != ACT_ENABLED) json_object_remove(body, "enabled");
        if (action != ACT_DAYS) json_object_remove(body, "syncDays");
        json_set_num(body, "id", id);
        store_call("update_mail_account", body, 0, s, write_done, id, &s->write);
    } else json_free(body);
    repaint(s);
}
static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt;
    MailScreen *s = (MailScreen *)base;
    if (!mail_settings_offered()) return;
    if (act == ACT_CANCEL || act == ACT_BROWSER_DONE) {
        if (s->write) return;
        stop_waiting(s); request_cancel(&s->read); KillTimer(pane_hwnd(base->pane), TIMER_MAIL); s->next_read = 0;
        set_text(&s->notice, act == ACT_CANCEL ? "Stopped waiting locally. You can start a new sign-in." : "Check the browser's result for success or a wrong-mailbox error; refreshing account status.");
        load(s); repaint(s); return;
    }
    if (act == ACT_FINISH) { finish(s); return; }
    if (act == ACT_CONNECT) { start(s, arg == 0 ? "gmail" : "outlook", 0); return; }
    const MailAccount *a = mail_account_find(&s->accounts, (int)arg);
    if (!a) return;
    int id = a->id;
    if (act == ACT_REAUTH) { start(s, a->provider, id); return; }
    if (act == ACT_LABEL || act == ACT_DAYS || act == ACT_ENABLED) { update(s, a, act); return; }
    const char *op = act == ACT_DELETE ? "delete_mail_account" : act == ACT_SYNC ? "sync_mail_account" : NULL;
    if (!op || !can_write(s, op)) return;
    if (act == ACT_DELETE) {
        modal_begin(s);
        bool confirmed = app_confirm("Disconnect mailbox?", "Core deletes its synced copy, its messages and saved tokens. Your provider's mailbox is kept. To revoke the app's access too, remove it in Google or Microsoft account settings.", "Disconnect", true);
        if (!modal_end(s)) return;
        if (!confirmed) return;
    }
    if (!can_write(s, op) || !mail_account_find(&s->accounts, id)) return;
    Json *body = json_object(); json_set_num(body, "id", id);
    store_call(op, body, 0, s, write_done, id, &s->write); repaint(s);
}
static void header(Screen *base, HeaderInfo *info) { (void)base; snprintf(info->title, sizeof info->title, "Mail account settings"); snprintf(info->subtitle, sizeof info->subtitle, "Global Gmail and Outlook mailboxes; read-only provider access"); }
static void layout(Screen *base, Doc *doc) {
    MailScreen *s = (MailScreen *)base; int x = px(20), w = doc->width - 2 * x;
    doc_space(doc, px(16));
    if (!mail_settings_offered()) { doc_notice(doc, x, w, "Mail settings are unavailable: this server must advertise the mail account route and this device needs an Admin token."); return; }
    if (s->error) { doc_notice(doc, x, w, s->error); doc_space(doc, px(10)); }
    if (s->notice) { doc_text(doc, x, w, s->notice, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); doc_space(doc, px(10)); }
    if (!s->loaded) { if (!s->blocked) doc_loading(doc, x, w, "Loading mail accounts..."); return; }
    if (s->sign_in.state) {
        ButtonSpec pending[] = {
            { 0, "Paste callback address", BUTTON_PROMINENT, ACT_FINISH, 0, !s->write && !s->sign_in.server_finish && GetTickCount64() >= s->retry_until && store_supports("finish_mail_account") },
            { 0, "Browser finished: refresh", BUTTON_PLAIN, ACT_BROWSER_DONE, 0, !s->write && s->sign_in.server_finish },
            { 0, "Cancel waiting", BUTTON_PLAIN, ACT_CANCEL, 0, !s->write },
        };
        doc_button_row(doc, x, w, pending, sizeof pending / sizeof *pending); doc_space(doc, px(12));
    }
    ButtonSpec connect[] = {
        { 0, "Connect Gmail", BUTTON_PROMINENT, ACT_CONNECT, 0, can_write(s, "connect_mail_account") },
        { 0, "Connect Outlook", BUTTON_PROMINENT, ACT_CONNECT, 1, can_write(s, "connect_mail_account") },
    };
    // Only configured providers appear; each operation also requires its deployed route.
    if (s->accounts.gmail && store_supports("connect_mail_account")) doc_button_row(doc, x, w, connect, 1);
    if (s->accounts.outlook && store_supports("connect_mail_account")) doc_button_row(doc, x, w, connect + 1, 1);
    if (!s->accounts.gmail && !s->accounts.outlook) doc_text(doc, x, w, "No mail providers are configured on this server.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(16));
    for (size_t i = 0; i < s->accounts.count; i++) {
        const MailAccount *a = &s->accounts.accounts[i];
        char *title = xstrfmt("%s (%s)", str_empty(a->label) ? a->email : a->label, a->email);
        doc_text(doc, x, w, title, FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK); free(title);
        char *last = a->last_sync_at ? format_relative((time_t)(a->last_sync_at / 1000)) : xstrdup("never");
        char *status = xstrfmt("%s | %s | %s | %d days | %d messages, %d unread | Last sync: %s%s", a->provider, a->status ? a->status : "unknown", a->enabled ? "Enabled" : "Disabled", a->sync_days, a->messages, a->unread, last, a->syncing ? " | Syncing..." : "");
        doc_text(doc, x, w, status, FONT_FOOTNOTE, theme.muted, DT_WORDBREAK); free(status); free(last);
        if (!str_empty(a->last_sync_error)) doc_notice(doc, x, w, a->last_sync_error);
        if (str_eq(a->status, "reauth")) doc_notice(doc, x, w, "Provider access expired or was revoked. Sign in again with this mailbox.");
        bool edit = can_write(s, "update_mail_account");
        ButtonSpec buttons[] = {
            { 0, "Label", BUTTON_PLAIN, ACT_LABEL, a->id, edit },
            { 0, a->enabled ? "Disable" : "Enable", BUTTON_PLAIN, ACT_ENABLED, a->id, edit },
            { 0, "Sync days", BUTTON_PLAIN, ACT_DAYS, a->id, edit },
            { 0, "Sign in again", BUTTON_PLAIN, ACT_REAUTH, a->id, can_write(s, "connect_mail_account") && mail_provider_available(&s->accounts, a->provider) },
            { 0, "Sync now", BUTTON_PLAIN, ACT_SYNC, a->id, can_write(s, "sync_mail_account") && !a->syncing && str_eq(a->status, "connected") },
            { 0, "Disconnect", BUTTON_PLAIN, ACT_DELETE, a->id, can_write(s, "delete_mail_account") },
        };
        const char *ops[] = { "update_mail_account", "update_mail_account", "update_mail_account", "connect_mail_account", "sync_mail_account", "delete_mail_account" };
        ButtonSpec offered[6]; size_t n = 0;
        for (size_t k = 0; k < sizeof buttons / sizeof *buttons; k++) if (store_supports(ops[k])) offered[n++] = buttons[k];
        doc_space(doc, px(6)); doc_button_row(doc, x, w, offered, n); doc_space(doc, px(20));
    }
    if (!s->accounts.count) doc_text(doc, x, w, "No connected mailboxes yet.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}
static void timer(Screen *base, UINT id) {
    if (id != TIMER_MAIL) return;
    MailScreen *s = (MailScreen *)base; KillTimer(pane_hwnd(base->pane), id);
    if (s->sign_in.state && now_ms() >= s->sign_in.expires_at) {
        stop_waiting(s); set_error(s, "Sign-in waiting expired. Check the browser result and refresh before starting again.", false);
    }
    load(s); repaint(s);
}
static void refresh(Screen *base) {
    MailScreen *s = (MailScreen *)base;
    if (s->write || GetTickCount64() < s->retry_until) return;
    s->next_read = 0;
    s->blocked = false; request_cancel(&s->read); set_error(s, NULL, false); load(s);
}
static void visible(Screen *base, bool shown) {
    MailScreen *s = (MailScreen *)base; s->shown = shown;
    if (shown) {
        s->blocked = false; s->next_read = 0;
        ULONGLONG tick = GetTickCount64();
        if (tick < s->retry_until) arm(s, (int)(s->retry_until - tick));
        else load(s);
    }
    else {
        KillTimer(pane_hwnd(base->pane), TIMER_MAIL); request_cancel(&s->read); request_cancel(&s->write);
        stop_waiting(s); set_text(&s->notice, NULL);
        // No retained account data crosses a hidden screen or account/server change.
        mail_accounts_free(&s->accounts); s->loaded = false;
    }
}
static void destroy(Screen *base) {
    MailScreen *s = (MailScreen *)base;
    if (base->pane) KillTimer(pane_hwnd(base->pane), TIMER_MAIL);
    request_cancel(&s->read); request_cancel(&s->write);
    if (s->modal) { s->retired = true; s->shown = false; return; }
    release(s);
}
static void activated(Screen *base, bool active) {
    MailScreen *s = (MailScreen *)base;
    if (!active) { KillTimer(pane_hwnd(base->pane), TIMER_MAIL); request_cancel(&s->read); }
    else if (s->shown && !s->modal) { s->next_read = 0; timer(base, TIMER_MAIL); }
}
static const ScreenVTable vt = { .destroy = destroy, .layout = layout, .header = header, .action = action, .timer = timer, .refresh = refresh, .visible = visible, .activated = activated };
Screen *mail_screen_new(void) { MailScreen *s = xcalloc(1, sizeof *s); s->base.vt = &vt; s->base.id = xstrdup("mail"); return &s->base; }
