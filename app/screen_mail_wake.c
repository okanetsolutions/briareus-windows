#include "screen_mail_wake_internal.h"

void mail_wake_release(MailWakeScreen *s) {
    for (int i = 0; i < MW_FIELDS; i++) if (s->edits[i]) DestroyWindow(s->edits[i]);
    for (int i = 0; i < MW_ROUTES; i++) { free(s->keys[i]); free(s->errors[i]); }
    mail_accounts_free(&s->accounts); mail_wake_free(&s->draft); session_free(&s->session);
    json_free(s->record); json_free(s->device); free(s->notice); api_client_release(s->client); screen_release(&s->base);
}
static void destroy(Screen *base) {
    MailWakeScreen *s = (MailWakeScreen *)base; mail_wake_cancel(s); s->shown = false;
    if (s->modal) { s->retired = true; return; }
    mail_wake_release(s);
}
static void visible(Screen *base, bool shown) {
    MailWakeScreen *s = (MailWakeScreen *)base; s->shown = shown; s->picker = -1;
    if (!shown || !mail_wake_current(s)) {
        mail_wake_cancel(s);
        for (int i = 0; i < MW_FIELDS; i++) if (s->edits[i]) ShowWindow(s->edits[i], SW_HIDE);
        memset(s->fresh, 0, sizeof s->fresh);
        mail_accounts_free(&s->accounts); s->tools = -1;
        return;
    }
    mail_wake_ensure_controls(s); mail_wake_load(s);
}
static void refresh(Screen *base) { mail_wake_load((MailWakeScreen *)base); }
// The confirmation can run a nested loop that retires this owner. Its caller must stop immediately on retirement.
bool mail_wake_confirm(MailWakeScreen *s) {
    s->modal = true; mail_wake_cancel(s);
    bool discard = app_confirm("Discard unsaved changes?", "The mail wake rules have not been saved.", "Discard", true);
    s->modal = false;
    return discard;
}
static bool can_leave(Screen *base) {
    MailWakeScreen *s = (MailWakeScreen *)base;
    if (s->modal) return false;
    if (!mail_wake_dirty(&s->draft)) return true;
    bool discard = mail_wake_confirm(s);
    if (s->retired) { mail_wake_release(s); return true; }
    if (discard) { mail_wake_discard(&s->draft); mail_wake_fill_controls(s); }
    return discard;
}
static void discard(MailWakeScreen *s, bool reconcile) {
    if (!mail_wake_editable(s)) return;
    if (reconcile && !mail_wake_read_current(s)) return;
    bool yes = mail_wake_confirm(s);
    if (s->retired) { mail_wake_release(s); return; }
    if (!yes || !s->shown || !mail_wake_current(s)) return;
    if (reconcile) {
        if (!mail_wake_read_current(s)) return;
        MailWake fresh = {0};
        if (!mail_wake_read(&fresh, s->record)) return;
        mail_wake_free(&s->draft); s->draft = fresh; s->draft_epoch = s->read_epoch;
        Json *e = (Json *)mail_wake_entry(s, MW_WRITE); json_set_bool(e, "uncertain", false);
        set_string(&s->errors[MW_WRITE], NULL);
    } else mail_wake_discard(&s->draft);
    mail_wake_fill_controls(s); mail_wake_repaint(s);
}
static void action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt; MailWakeScreen *s = (MailWakeScreen *)base;
    if (act == MW_RETRY) { mail_wake_load(s); return; }
    if (act == MW_SAVE) { mail_wake_save(s); return; }
    if (act == MW_DISCARD || act == MW_RECONCILE) { discard(s, act == MW_RECONCILE); return; }
    if (!mail_wake_editable(s)) return;
    if (act == MW_FOCUS) { if (arg >= 0 && arg < MW_FIELDS && s->edits[arg]) SetFocus(s->edits[arg]); return; }
    if (act == MW_ADD) {
        int only = 0, count = 0;
        if (!s->fresh[MW_ACCOUNTS] || !s->fresh[MW_PROJECTS] || s->tools != 1) return;
        for (size_t i = 0; i < s->accounts.count; i++) {
            int id = s->accounts.accounts[i].id;
            if (mail_wake_mailbox_usable(&s->accounts, id)) { only = id; count++; }
        }
        if (!count) return;
        mail_wake_add(&s->draft, count == 1 ? only : 0); mail_wake_fill_controls(s);
    } else if (act == MW_SELECT) {
        if (s->picker < 0 || (size_t)s->picker >= json_count(s->draft.rules)) return;
        if (!s->fresh[MW_ACCOUNTS] || !mail_wake_mailbox_usable(&s->accounts, (int)arg)) return;
        json_set_num((Json *)json_at(s->draft.rules, (size_t)s->picker), "account", (double)arg); s->picker = -1;
    } else {
        if (arg < 0 || (size_t)arg >= json_count(s->draft.rules)) return;
        Json *r = (Json *)json_at(s->draft.rules, (size_t)arg);
        if (act == MW_PICK) { s->picker = s->picker == arg ? -1 : (int)arg; }
        if (act == MW_REMOVE) { mail_wake_remove(&s->draft, (size_t)arg); s->picker = -1; mail_wake_fill_controls(s); }
        if (act == MW_TOGGLE) {
            bool on = json_bool_is(json_get(r, "enabled"), true);
            int account = json_int_or(json_get(r, "account"), 0);
            if (!on && (!s->fresh[MW_PROJECTS] || !s->fresh[MW_ACCOUNTS] || s->tools != 1)) return;
            if (!on && !mail_wake_mailbox_usable(&s->accounts, account)) return;
            json_set_bool(r, "enabled", !on);
        }
    }
    mail_wake_repaint(s);
}
static void command(Screen *base, int id, int code, HWND control) {
    (void)control; MailWakeScreen *s = (MailWakeScreen *)base;
    int field = id - MW_FIELD_ID;
    if (field < 0 || field >= MW_FIELDS || s->filling || !mail_wake_editable(s)) return;
    if (code == EN_CHANGE) {
        bool dirty = mail_wake_dirty(&s->draft);
        mail_wake_capture(s, field);
        if (dirty != mail_wake_dirty(&s->draft)) mail_wake_repaint(s);
        else pane_header_changed(base->pane);
    }
}
static bool key(Screen *base, WPARAM vk, bool ctrl, bool shift) {
    (void)shift;
    if (ctrl && vk == 'S') { mail_wake_save((MailWakeScreen *)base); return true; }
    return false;
}
static void activated(Screen *base, bool active) {
    MailWakeScreen *s = (MailWakeScreen *)base;
    if (!active) { for (int i = 0; i < MW_WRITE; i++) request_cancel(&s->requests[i]); }
    else mail_wake_load(s);
}
static const ScreenVTable VT = {
    .destroy = destroy, .visible = visible, .refresh = refresh, .activated = activated, .can_leave = can_leave,
    .action = action, .command = command, .key = key, .layout = mail_wake_layout, .header = mail_wake_header,
    .place = mail_wake_place, .effective_visibility = true,
};
Screen *mail_wake_screen_new(const Session *session) {
    MailWakeScreen *s = xcalloc(1, sizeof *s); s->base.vt = &VT; s->tools = s->picker = -1;
    session_copy(&s->session, session); s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    char *scope = json_serialize(s->device, true);
    for (int i = 0; i < MW_ROUTES; i++) {
        s->keys[i] = xstrfmt("%s:%s:%s:%s", scope, mail_wake_operations[i],
            i == MW_READ || i == MW_WRITE ? (session_repo(session) ? session_repo(session) : "") : "", i == MW_READ || i == MW_WRITE ? session_id(session) : "");
    }
    free(scope); s->base.id = xstrfmt("mail-wake:%p:%s", (void *)s->client, s->keys[MW_READ]); return &s->base;
}
