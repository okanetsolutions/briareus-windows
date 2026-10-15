#include "screen_mail_internal.h"

static void failed(MailScreen *s, Request *r, bool screen_mail_finish);
static void read_done(void *owner, Request *r);
static void start_done(void *owner, Request *r);

void screen_mail_arm(MailScreen *s, int delay) {
    if (!s->shown || s->blocked || s->modal || !g_store.active || !mail_settings_offered()) return;
    s->next_read = GetTickCount64() + (ULONGLONG)delay;
    SetTimer(pane_hwnd(s->base.pane), TIMER_MAIL, (UINT)delay, NULL);
}

bool screen_mail_syncing(MailScreen *s) { for (size_t i = 0; i < s->accounts.count; i++) if (s->accounts.accounts[i].syncing) return true; return false; }

void screen_mail_stop_waiting(MailScreen *s) { mail_sign_in_free(&s->sign_in); }

static void failed(MailScreen *s, Request *r, bool screen_mail_finish) {
    screen_mail_set_error(s, mail_error_message(r->error.status, screen_mail_finish, r->error.message), str_eq(r->operation, "settings_mail_accounts"));
    if (r->error.status == 401 || r->error.status == 403 || (r->error.status == 404 && s->read_error)) s->blocked = true;
    if (r->error.status == 429) {
        s->failures++;
        int delay = mail_settings_retry_ms(s->failures, r->error.retry_after);
        s->retry_until = GetTickCount64() + (ULONGLONG)delay;
        screen_mail_arm(s, delay);
    }
}

static void read_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!screen_mail_current(s, r)) return;
    MailAccounts fresh;
    if (!r->ok) {
        failed(s, r, false);
        if (!s->blocked && r->error.status != 429) {
            s->failures++;
            int delay = mail_settings_retry_ms(s->failures, r->error.retry_after);
            screen_mail_arm(s, delay);
        }
    } else if (!mail_accounts_parse(r->result, &fresh)) {
        screen_mail_set_error(s, "The server returned an unexpected mail account list.", true);
        s->blocked = true;
    } else {
        if (s->read_error) screen_mail_set_error(s, NULL, false);
        s->loaded = true; s->failures = 0; s->next_read = 0; s->retry_until = 0;
        if (s->sign_in.state && mail_sign_in_completed(&s->sign_in, &fresh)) {
            screen_mail_stop_waiting(s); screen_mail_set_text(&s->notice, "The account list now reflects a connected mailbox.");
        }
        mail_accounts_free(&s->accounts); s->accounts = fresh;
        settings_mail_changed(r->result);
        screen_mail_label_fill(s);
        if (s->sign_in.state || screen_mail_syncing(s)) screen_mail_arm(s, 10000);
    }
    screen_mail_repaint(s);
}

void screen_mail_load(MailScreen *s) {
    if (!s->shown || s->blocked || s->modal) return;
    if (s->read || s->write || !g_store.active || !mail_settings_offered()) return;
    ULONGLONG tick = GetTickCount64();
    if (tick < s->retry_until) { screen_mail_arm(s, (int)(s->retry_until - tick)); return; }
    if (tick < s->next_read) { screen_mail_arm(s, (int)(s->next_read - tick)); return; }
    store_call("settings_mail_accounts", json_object(), 0, s, read_done, 0, &s->read);
    screen_mail_repaint(s);
}

static void start_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!screen_mail_current(s, r)) return;
    if (!r->ok) failed(s, r, false);
    else if (!mail_sign_in_parse(r->result, s->starting_provider, s->starting_id, &s->accounts, &s->sign_in)
             || screen_mail_now_ms() >= s->sign_in.expires_at
             || (!s->sign_in.server_finish && !store_supports("finish_mail_account"))) {
        screen_mail_stop_waiting(s); screen_mail_set_error(s, "The server returned an invalid or expired sign-in. Start again.", false);
    } else {
        screen_mail_set_error(s, NULL, false);
        screen_mail_set_text(&s->notice, s->sign_in.server_finish
            ? "Finish sign-in in the browser. This page checks every 10 seconds. The browser reports errors, including a different mailbox (409). For an already connected mailbox, confirm the browser result then refresh here."
            : "Finish sign-in in the browser, then immediately paste the complete final callback address here; Microsoft codes may expire within a minute.");
        open_web_url(json_str(json_get(r->result, "url")));
        screen_mail_arm(s, 10000);
    }
    if (!s->sign_in.state && (screen_mail_syncing(s) || (!r->ok && r->error.status == 404))) {
        if (!r->ok && r->error.status == 404) s->next_read = 0;
        screen_mail_load(s);
    }
    screen_mail_repaint(s);
}

void screen_mail_start(MailScreen *s, const char *provider, int id, bool manage) {
    if (!screen_mail_can_write(s, "connect_mail_account") || !s->loaded || !mail_provider_available(&s->accounts, provider)) return;
    Json *body = json_object(); json_set_str(body, "provider", provider);
    if (id) json_set_num(body, "accountId", id);
    if (manage) json_set_str(body, "access", "manage");
    // Let the server apply defaults only to new mailboxes and preserve settings on reconnect.
    screen_mail_set_text(&s->starting_provider, provider); s->starting_id = id;
    screen_mail_set_error(s, NULL, false); screen_mail_set_text(&s->notice, NULL);
    store_call("connect_mail_account", body, 0, s, start_done, 0, &s->write); screen_mail_repaint(s);
}

void screen_mail_write_done(void *owner, Request *r) {
    MailScreen *s = owner;
    if (!screen_mail_current(s, r)) return;
    bool screen_mail_finish = str_eq(r->operation, "finish_mail_account");
    bool label_write = s->label_write;
    if (screen_mail_finish) { screen_mail_stop_waiting(s); screen_mail_set_text(&s->notice, NULL); } // Exchanges are single-use, including a refusal or ambiguous network failure.
    char *screen_mail_saved_label = NULL;
    if (!r->ok) { failed(s, r, screen_mail_finish); }
    else {
        bool deletion = str_eq(r->operation, "delete_mail_account");
        MailAccount a = {0};
        bool valid = deletion ? json_bool_is(json_get(r->result, "ok"), true) : mail_account_parse(json_get(r->result, "account"), &a);
        if (valid && !deletion) {
            if (!screen_mail_finish && a.id != r->tag) valid = false;
            if (screen_mail_finish && s->starting_id && a.id != s->starting_id) valid = false;
            if (valid && label_write) screen_mail_saved_label = xstrdup(a.label ? a.label : "");
            mail_account_free(&a);
        }
        if (!valid) screen_mail_set_error(s, "The server returned an unexpected mail response. Refresh before trying again.", false);
        else {
            screen_mail_set_error(s, NULL, false);
            screen_mail_set_text(&s->notice, str_eq(r->operation, "sync_mail_account")
                ? "Sync started (202); polling account status until it finishes."
                : deletion ? "Disconnected. The provider may still list this app as having access; remove it there to revoke access."
                : screen_mail_finish ? "Mailbox connected; refreshing status."
                : "Account settings saved; refreshing status.");
        }
    }
    if (label_write) {
        s->label_write = false;
        // A keystroke after Save started is kept. The box is saved only when it still matches the answer.
        if (screen_mail_saved_label && s->label_edit) {
            char *now = screen_mail_edit_text(s->label_edit), *trim = str_trim(now);
            if (str_eq(trim, screen_mail_saved_label)) {
                s->filling = true; screen_mail_set_edit_text(s->label_edit, screen_mail_saved_label); s->filling = false; s->dirty = false;
            }
            free(now); free(trim);
        }
        free(screen_mail_saved_label);
        pane_header_changed(s->base.pane);
    }
    if (!s->blocked && GetTickCount64() >= s->retry_until) { s->next_read = 0; screen_mail_load(s); }
    screen_mail_repaint(s);
}

// A token revocation can replace the screen inside a modal dialog's nested message loop.
// Retain the screen until the dialog returns, then discard its input if it was retired.
void screen_mail_modal_begin(MailScreen *s) {
    s->modal = true; KillTimer(pane_hwnd(s->base.pane), TIMER_MAIL); request_cancel(&s->read); s->next_read = 0;
}

bool screen_mail_modal_end(MailScreen *s) {
    s->modal = false;
    if (s->retired) { screen_mail_release(s); return false; }
    if (s->sign_in.state || screen_mail_syncing(s)) screen_mail_arm(s, 10000);
    return true;
}

void screen_mail_finish(MailScreen *s) {
    if (!s->sign_in.state || s->sign_in.server_finish || s->write || GetTickCount64() < s->retry_until || !store_supports("finish_mail_account")) return;
    screen_mail_modal_begin(s);
    char *url = dialog_text(pane_hwnd(s->base.pane), "Finish mail sign-in", "Paste the complete callback address immediately after sign-in", "Finish", "");
    if (!screen_mail_modal_end(s)) { free(url); return; }
    if (!url) return;
    if (!s->shown || s->blocked || GetTickCount64() < s->retry_until || !store_supports("finish_mail_account")) { free(url); return; }
    Json *body = mail_sign_in_finish(&s->sign_in, url, screen_mail_now_ms()); free(url);
    if (!body) { screen_mail_set_error(s, "Callback does not match this sign-in, contains an error, or has expired. Check the final address or start again.", false); screen_mail_repaint(s); return; }
    request_cancel(&s->read); KillTimer(pane_hwnd(s->base.pane), TIMER_MAIL);
    s->next_read = 0;
    store_call("finish_mail_account", body, 0, s, screen_mail_write_done, s->starting_id, &s->write); screen_mail_repaint(s);
}

void screen_mail_update(MailScreen *s, const MailAccount *a, int screen_mail_action) {
    if (!screen_mail_can_write(s, "update_mail_account")) return;
    // Dialogs run a nested message loop: copy the id and values before status polling can replace the list.
    int id = a->id; bool enabled = a->enabled;
    char *label = xstrdup(a->label ? a->label : ""), *days = xstrfmt("%d", a->sync_days);
    Json *body = NULL;
    if (screen_mail_action == ACT_DAYS) {
        screen_mail_modal_begin(s);
        char *v = dialog_text(pane_hwnd(s->base.pane), "Mail sync window", "Days to keep (1-365); changing this restarts the first sync", "Save", days);
        if (!screen_mail_modal_end(s)) { free(v); free(label); free(days); return; }
        if (v) { body = mail_settings_body(label, enabled, v); if (!body) screen_mail_set_error(s, "Enter a whole number of days from 1 to 365.", false); free(v); }
    } else body = mail_settings_body(label, !enabled, days);
    free(label); free(days);
    if (body && screen_mail_can_write(s, "update_mail_account") && mail_account_find(&s->accounts, id)) {
        // Send only the field edited. The label is the page's own field, saved with Save.
        json_object_remove(body, "label");
        if (screen_mail_action != ACT_ENABLED) json_object_remove(body, "enabled");
        if (screen_mail_action != ACT_DAYS) json_object_remove(body, "syncDays");
        json_set_num(body, "id", id);
        store_call("update_mail_account", body, 0, s, screen_mail_write_done, id, &s->write);
    } else json_free(body);
    screen_mail_repaint(s);
}
