#include "screen_mail_internal.h"

static void timer(Screen *base, UINT id);
static void refresh(Screen *base);
static bool can_leave(Screen *base);
static void destroy(Screen *base);
static void activated(Screen *base, bool active);
static Screen *open_mail_settings(int account_id);

bool mail_settings_offered(void) { return g_store.has_device && permission_rank(g_store.device.permission) == 2 && store_supports("settings_mail_accounts"); }

double screen_mail_now_ms(void) { FILETIME ft; GetSystemTimeAsFileTime(&ft); ULARGE_INTEGER n; n.LowPart = ft.dwLowDateTime; n.HighPart = ft.dwHighDateTime; return (double)(n.QuadPart / 10000ULL - 11644473600000ULL); }

void screen_mail_set_text(char **slot, const char *text) { free(*slot); *slot = xstrdup(text); }

bool mail_settings_result_current(bool shown, const Request *r) { return shown && !r->cancelled && r->client && r->client == g_store.client && mail_settings_offered(); }

int mail_settings_retry_ms(int failures, double retry_after) {
    int delay = 10000;
    for (int i = 1; i < failures && delay < 60000; i++) delay = delay > 30000 ? 60000 : delay * 2;
    if (isfinite(retry_after) && retry_after > (double)delay / 1000) {
        delay = retry_after >= (double)INT_MAX / 1000 ? INT_MAX : (int)ceil(retry_after * 1000);
    }
    return delay;
}

void screen_mail_set_error(MailScreen *s, const char *text, bool from_read) {
    screen_mail_set_text(&s->error, text); s->read_error = from_read;
}

bool screen_mail_current(MailScreen *s, Request *r) { return mail_settings_result_current(s->shown, r); }

bool screen_mail_can_write(MailScreen *s, const char *op) { return s->shown && !s->blocked && !s->write && !s->sign_in.state && !s->read && GetTickCount64() >= s->retry_until && store_supports(op); }

void screen_mail_repaint(MailScreen *s) { pane_relayout(s->base.pane); pane_header_changed(s->base.pane); }

void screen_mail_release(MailScreen *s) {
    if (s->label_edit) { DestroyWindow(s->label_edit); s->label_edit = NULL; }
    mail_sign_in_free(&s->sign_in); mail_accounts_free(&s->accounts);
    free(s->error); free(s->notice); free(s->starting_provider); screen_release(&s->base);
}

void screen_mail_action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt;
    MailScreen *s = (MailScreen *)base;
    if (!mail_settings_offered()) return;
    if (act == ACT_CANCEL || act == ACT_BROWSER_DONE) {
        if (s->write) return;
        screen_mail_stop_waiting(s); request_cancel(&s->read); KillTimer(pane_hwnd(base->pane), TIMER_MAIL); s->next_read = 0;
        screen_mail_set_text(&s->notice, act == ACT_CANCEL ? "Stopped waiting locally. You can start a new sign-in." : "Check the browser's result for success or a wrong-mailbox error; refreshing account status.");
        screen_mail_load(s); screen_mail_repaint(s); return;
    }
    if (act == ACT_FINISH) { screen_mail_finish(s); return; }
    if (act == ACT_CONNECT) { screen_mail_start(s, arg == 0 ? "gmail" : "outlook", 0, false); return; }
    if (act == ACT_SAVE) { screen_mail_label_save(s); return; }
    if (act == ACT_FOCUS) { if (s->label_edit && IsWindowEnabled(s->label_edit)) SetFocus(s->label_edit); return; }
    const MailAccount *a = mail_account_find(&s->accounts, (int)arg);
    if (!a) return;
    int id = a->id;
    if (act == ACT_REAUTH || act == ACT_MANAGE) { screen_mail_start(s, a->provider, id, act == ACT_MANAGE); return; }
    if (act == ACT_DAYS || act == ACT_ENABLED) { screen_mail_update(s, a, act); return; }
    const char *op = act == ACT_DELETE ? "delete_mail_account" : act == ACT_SYNC ? "sync_mail_account" : NULL;
    if (!op || !screen_mail_can_write(s, op)) return;
    if (act == ACT_DELETE) {
        screen_mail_modal_begin(s);
        bool confirmed = app_confirm("Disconnect mailbox?", "Core deletes its synced copy, its messages and saved tokens. Your provider's mailbox is kept. To revoke the app's access too, remove it in Google or Microsoft account settings.", "Disconnect", true);
        if (!screen_mail_modal_end(s)) return;
        if (!confirmed) return;
    }
    if (!screen_mail_can_write(s, op) || !mail_account_find(&s->accounts, id)) return;
    Json *body = json_object(); json_set_num(body, "id", id);
    store_call(op, body, 0, s, screen_mail_write_done, id, &s->write); screen_mail_repaint(s);
}

static void timer(Screen *base, UINT id) {
    if (id != TIMER_MAIL) return;
    MailScreen *s = (MailScreen *)base; KillTimer(pane_hwnd(base->pane), id);
    if (s->sign_in.state && screen_mail_now_ms() >= s->sign_in.expires_at) {
        screen_mail_stop_waiting(s); screen_mail_set_text(&s->notice, NULL);
        screen_mail_set_error(s, "Sign-in waiting expired. Check the browser result and refresh before starting again.", false);
    }
    // Paste-back sign-ins need expiry checks, not account reads that can impose a callback-blocking backoff.
    if (s->sign_in.state && !s->sign_in.server_finish && !screen_mail_syncing(s) && !s->retry_until) screen_mail_arm(s, 10000);
    else screen_mail_load(s);
    screen_mail_repaint(s);
}

static void refresh(Screen *base) {
    MailScreen *s = (MailScreen *)base;
    if (s->write || GetTickCount64() < s->retry_until) return;
    s->next_read = 0;
    s->blocked = false; request_cancel(&s->read); screen_mail_set_error(s, NULL, false); screen_mail_load(s);
}

static bool can_leave(Screen *base) {
    MailScreen *s = (MailScreen *)base;
    if (s->modal) return false;
    if (!s->dirty) return true;
    const MailAccount *a = mail_account_find(&s->accounts, s->focus);
    const char *name = a && !str_empty(a->label) ? a->label : a && a->email ? a->email : "this mailbox";
    char *message = xstrfmt("The changes to %s have not been saved.", name);
    // Pause reads for the dialog. A screen retired inside it is left for the caller to destroy.
    s->modal = true; KillTimer(pane_hwnd(base->pane), TIMER_MAIL); request_cancel(&s->read); s->next_read = 0;
    bool leave = app_confirm("Discard unsaved changes?", message, "Discard", true);
    free(message);
    s->modal = false;
    if (s->retired) return true;
    if (leave) s->dirty = false;
    else if (s->sign_in.state || screen_mail_syncing(s)) screen_mail_arm(s, 10000);
    return leave;
}

void screen_mail_visible(Screen *base, bool shown) {
    MailScreen *s = (MailScreen *)base; s->shown = shown;
    if (shown) {
        s->blocked = false; s->next_read = 0;
        screen_mail_label_ensure(s);
        ULONGLONG tick = GetTickCount64();
        if (tick < s->retry_until) screen_mail_arm(s, (int)(s->retry_until - tick));
        else screen_mail_load(s);
    }
    else {
        KillTimer(pane_hwnd(base->pane), TIMER_MAIL); request_cancel(&s->read); request_cancel(&s->write);
        screen_mail_stop_waiting(s); screen_mail_set_text(&s->notice, NULL);
        s->dirty = false; s->label_write = false;
        if (s->label_edit) { s->filling = true; screen_mail_set_edit_text(s->label_edit, ""); s->filling = false; ShowWindow(s->label_edit, SW_HIDE); }
        // No retained account data crosses a hidden screen or account/server change.
        mail_accounts_free(&s->accounts); s->loaded = false;
    }
}

static void destroy(Screen *base) {
    MailScreen *s = (MailScreen *)base;
    if (base->pane) KillTimer(pane_hwnd(base->pane), TIMER_MAIL);
    request_cancel(&s->read); request_cancel(&s->write);
    if (s->modal) { s->retired = true; s->shown = false; return; }
    screen_mail_release(s);
}

static void activated(Screen *base, bool active) {
    MailScreen *s = (MailScreen *)base;
    if (!active) { KillTimer(pane_hwnd(base->pane), TIMER_MAIL); request_cancel(&s->read); }
    else if (s->shown && !s->modal) { s->next_read = 0; timer(base, TIMER_MAIL); }
}

static const ScreenVTable vt = {
    .destroy = destroy, .layout = screen_mail_layout, .header = screen_mail_header, .action = screen_mail_action, .timer = timer, .refresh = refresh,
    .place = screen_mail_place, .visible = screen_mail_visible, .command = screen_mail_command, .key = screen_mail_key, .can_leave = can_leave, .activated = activated,
};

static Screen *open_mail_settings(int account_id) {
    MailScreen *s = xcalloc(1, sizeof *s);
    s->base.vt = &vt;
    s->focus = account_id > 0 ? account_id : 0;
    s->base.id = s->focus ? xstrfmt("mail-settings:%d", s->focus) : xstrdup("mail-settings");
    return &s->base;
}

Screen *mail_settings_screen_new(void) { return open_mail_settings(0); }

Screen *mail_settings_screen_open(int account_id) { return open_mail_settings(account_id); }
