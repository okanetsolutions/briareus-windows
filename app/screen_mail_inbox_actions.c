#include "screen_mail_inbox_internal.h"

static void delete_done(void *owner, Request *r);
static void delete_message(Inbox *s);

void screen_mail_inbox_edit_filter(Inbox *s, int act) {
    if (!screen_mail_inbox_window(s)) return;
    char **slot = act == ACT_Q ? &s->filter.q : act == ACT_LABEL ? &s->filter.label : &s->filter.thread;
    const char *label = act == ACT_Q ? "Search sender, subject or snippet (200 characters maximum)" : act == ACT_LABEL ? "Exact label or folder name" : "Exact thread/conversation ID";
    s->modal = true; screen_mail_inbox_cancel(s);
    char *value = dialog_text(screen_mail_inbox_window(s), "Mail filter", label, "Apply", *slot ? *slot : "");
    s->modal = false;
    if (s->retired) { free(value); screen_mail_inbox_release(s); return; }
    if (!s->shown || !mail_inbox_offered()) { free(value); screen_mail_inbox_clear_private(s); screen_mail_inbox_repaint(s); return; }
    if (value) {
        free(*slot); *slot = value;
        // Any, Inbox, and Outside already drop the label. Applying a label is the other alternative.
        if (act == ACT_LABEL) s->filter.inbox = -1;
        screen_mail_inbox_reload(s);
    } else { if (!s->accounts_loaded || !s->loaded) screen_mail_inbox_load_accounts(s); if (s->selected_id && !s->body.id) screen_mail_inbox_load_body(s); screen_mail_inbox_arm(s); }
}

void screen_mail_inbox_open_provider(Inbox *s) {
    if (s->body.id && mail_account_readable(&s->accounts, s->body.account_id) && mail_message_web_url_safe(s->body.web_url))
        open_web_url(s->body.web_url);
}

void screen_mail_inbox_cycle_account(Inbox *s) {
    int cap = (int)s->accounts.count + 1;
    int *ids = xcalloc((size_t)cap, sizeof *ids), n = 0, cur = 0;
    ids[n++] = 0;
    for (size_t i = 0; i < s->accounts.count; i++)
        if (mail_account_readable(&s->accounts, s->accounts.accounts[i].id)) ids[n++] = s->accounts.accounts[i].id;
    for (int i = 0; i < n; i++) if (ids[i] == s->filter.account) cur = i;
    s->filter.account = ids[(cur + 1) % n];
    free(ids);
    screen_mail_inbox_reload(s);
}

void screen_mail_inbox_move_focus(Inbox *s, int delta) {
    MailGroups groups = mail_groups_build(&s->messages, s->group == GROUP_THREADS);
    MailGroup *rows = groups.groups; size_t n = groups.count;
    int *shown = xcalloc(n ? n : 1, sizeof *shown); int sn = 0;
    for (size_t i = 0; i < n; i++) if (screen_mail_inbox_convo_shown(s, &rows[i])) shown[sn++] = rows[i].primary;
    mail_groups_free(&groups);
    if (!sn) { free(shown); return; }
    int at = -1;
    for (int i = 0; i < sn; i++) if (shown[i] == s->focus_message) at = i;
    if (at < 0) at = delta > 0 ? 0 : sn - 1;
    else { at += delta; if (at < 0) at = 0; if (at >= sn) at = sn - 1; }
    s->focus_message = shown[at];
    s->reveal_focus = true; s->reveal_open = false;
    free(shown);
    screen_mail_inbox_repaint(s);
}

static void delete_done(void *owner, Request *r) {
    Inbox *s = owner;
    if (!screen_mail_inbox_current(s, r)) return;
    int account = json_int_or(json_get(r->args, "account"), 0);
    const char *id = json_str(json_get(r->args, "id"));
    if (r->ok && json_bool_is(json_get(r->result, "ok"), true)) {
        g_store.mail_failures = 0;
        for (size_t i = 0; i < s->messages.count; i++) {
            MailMessage *m = &s->messages.messages[i];
            if (m->account_id != account || !str_eq(m->id, id)) continue;
            mail_message_free(m);
            memmove(m, m + 1, (s->messages.count - i - 1) * sizeof *m);
            s->messages.count--;
            break;
        }
        screen_mail_inbox_clear_selection(s); s->focus_message = -1;
        screen_mail_inbox_text_set(&s->notice, "Message moved to trash.");
        screen_mail_inbox_load_accounts(s);
    } else if (!r->ok && (r->error.status == 401 || r->error.status == 403)) {
        screen_mail_inbox_failure(s, r, &s->body_error, true);
    } else if (!r->ok && r->error.status == 404) {
        screen_mail_inbox_reset_list(s); screen_mail_inbox_text_set(&s->notice, "This message or mailbox is no longer available."); screen_mail_inbox_load_accounts(s);
    } else {
        if (!r->ok && (r->error.status == 429 || r->error.status == 0 || r->error.status >= 500))
            screen_mail_inbox_failure(s, r, &s->body_error, true);
        const char *message = !r->ok && r->error.status == 409
            ? "Choose Enable mail actions for this mailbox in Settings > Mail, approve access, then try again."
            : !r->ok && r->error.status == 429
            ? "The mail provider is busy. Wait before trying deletion again."
            : "Deletion could not be confirmed. Refresh mail to check whether the message moved to trash before trying again.";
        screen_mail_inbox_text_set(&s->body_error, message);
    }
    screen_mail_inbox_arm(s); screen_mail_inbox_repaint(s);
}

static void delete_message(Inbox *s) {
    if (!screen_mail_inbox_ready(s) || !s->body.id || !store_supports("delete_mail_message")
        || !mail_account_readable(&s->accounts, s->body.account_id)) return;
    int account = s->body.account_id, generation = s->generation;
    char *id = xstrdup(s->body.id);
    ApiClient *client = g_store.client; api_client_retain(client);
    char *question = xstrfmt("Move this message to trash?\n\n%s\n\nOnly this message will be moved, not the whole conversation. You can restore it in your mailbox.", screen_mail_inbox_subject_of(&s->body));
    s->modal = true; screen_mail_inbox_cancel(s);
    bool confirmed = app_confirm("Delete message?", question, "Move to trash", true);
    free(question); s->modal = false;
    if (s->retired) { free(id); api_client_release(client); screen_mail_inbox_release(s); return; }
    bool valid = confirmed && screen_mail_inbox_ready(s) && g_store.client == client && s->generation == generation
        && s->selected_account == account && str_eq(s->selected_id, id)
        && mail_account_readable(&s->accounts, account) && store_supports("delete_mail_message");
    api_client_release(client);
    if (valid) {
        Json *args = json_object(); json_set_num(args, "account", account); json_set_str(args, "id", id);
        screen_mail_inbox_text_set(&s->body_error, NULL); screen_mail_inbox_text_set(&s->notice, NULL);
        store_call("delete_mail_message", args, 0, s, delete_done, s->generation, &s->deletion);
    } else { screen_mail_inbox_load_accounts(s); if (s->selected_id && !s->body.id) screen_mail_inbox_load_body(s); }
    free(id); screen_mail_inbox_arm(s); screen_mail_inbox_repaint(s);
}

void screen_mail_inbox_action(Screen *base, int act, intptr_t arg, POINT pt) {
    (void)pt; Inbox *s = (Inbox *)base;
    if (!s->shown || s->modal || s->deletion || !mail_inbox_offered()) return;
    if (act >= ACT_REPLY && act <= ACT_REPLY_RECONCILE) { screen_mail_reply_action(s, act); return; }
    if (act == ACT_DELETE) { delete_message(s); return; }
    if (act == ACT_MENU) {
        s->menu = s->menu == (int)arg ? MENU_NONE : (int)arg;
        if (s->base.pane) s->scroll_seen = pane_scroll_y(s->base.pane);
        screen_mail_inbox_repaint(s); return;
    }
    s->menu = MENU_NONE;
    if (act == ACT_Q || act == ACT_LABEL || act == ACT_THREAD) { screen_mail_inbox_edit_filter(s, act); return; }
    if (act == ACT_RESET) { mail_filter_free(&s->filter); s->group = GROUP_THREADS; screen_mail_inbox_reload(s); return; }
    if (act == ACT_ACCOUNT) {
        if (arg && !mail_account_readable(&s->accounts, (int)arg)) return;
        s->filter.account = (int)arg; screen_mail_inbox_reload(s); return;
    }
    if (act == ACT_KIND) { if (arg < KIND_PEOPLE || arg > KIND_ALL) return; s->kind = (int)arg; screen_mail_inbox_repaint(s); return; }
    if (act == ACT_GROUP) { if (arg != GROUP_THREADS && arg != GROUP_EACH) return; s->group = (int)arg; screen_mail_inbox_repaint(s); return; }
    if (act == ACT_UNREAD) { s->filter.unread = s->filter.unread == 1 ? -1 : 1; screen_mail_inbox_reload(s); return; }
    if (act == ACT_STARRED) { s->filter.starred = s->filter.starred == 1 ? -1 : 1; screen_mail_inbox_reload(s); return; }
    if (act == ACT_INBOX) {
        if (arg < -1 || arg > 1) return;
        s->filter.inbox = (int)arg; free(s->filter.label); s->filter.label = NULL; screen_mail_inbox_reload(s); return;
    }
    if (act == ACT_SYNC) { if (screen_mail_inbox_ready(s)) screen_mail_inbox_reload(s); return; }
    if (act == ACT_CLOSE) { screen_mail_inbox_clear_selection(s); screen_mail_inbox_repaint(s); return; }
    if (act == ACT_PROVIDER) { screen_mail_inbox_open_provider(s); return; }
    if (act == ACT_DELETE_PROVIDER) {
        if (!s->body.id || !mail_account_readable(&s->accounts, s->body.account_id) || !mail_message_web_url_safe(s->body.web_url)) return;
        screen_mail_inbox_text_set(&s->limit_note, "Delete this message in the mailbox that opens. After the next mail sync, click Refresh to update this list.");
        screen_mail_inbox_open_provider(s); screen_mail_inbox_repaint(s); return;
    }
    if (act == ACT_MORE || act == ACT_RETRY_LIST) { if (!s->accounts_loaded) screen_mail_inbox_load_accounts(s); else screen_mail_inbox_load_list(s); return; }
    if (act == ACT_RETRY_ACCOUNTS) { screen_mail_inbox_load_accounts(s); return; }
    if (act == ACT_RETRY_BODY) { screen_mail_inbox_load_body(s); return; }
    if (act == ACT_SELECT && arg >= 0 && (size_t)arg < s->messages.count && store_supports("mail_message")) {
        const MailMessage *m = &s->messages.messages[arg];
        if (!mail_account_readable(&s->accounts, m->account_id)) return;
        int primary = screen_mail_inbox_convo_primary_for(s, (int)arg);
        screen_mail_inbox_clear_selection(s); s->selected_id = xstrdup(m->id); s->selected_account = m->account_id;
        s->focus_message = primary; s->reveal_open = true; s->reveal_focus = false; s->reveal_block = true;
        screen_mail_inbox_text_set(&s->limit_note, NULL);
        screen_mail_inbox_load_body(s); screen_mail_inbox_repaint(s);
    }
}

void screen_mail_inbox_header(Screen *base, HeaderInfo *info) {
    (void)base; snprintf(info->title, sizeof info->title, "Mail"); info->subtitle[0] = 0;
}
