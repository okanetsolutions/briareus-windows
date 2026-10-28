#include "screen_mail_inbox_internal.h"

MailReply *screen_mail_reply_draft(Inbox *s, bool create) {
    if (s->mail_client != g_store.client) return NULL;
    if (!s->body.id || s->selected_account != s->body.account_id || !str_eq(s->selected_id, s->body.id)) return NULL;
    char *key = mail_reply_key(&s->accounts, &s->body);
    MailReply *draft = mail_reply_find(&g_store.mail_replies, key, create);
    free(key); return draft;
}

bool screen_mail_reply_allowed(Inbox *s) {
    return s->mail_client == g_store.client && screen_mail_inbox_ready(s) && store_supports("reply_mail_message")
        && mail_account_writable(&s->accounts, s->body.account_id);
}

typedef struct { char *key; } ReplyCompletion;
static void reply_completed(void *ctx, Request *r) {
    ReplyCompletion *completion = ctx;
    // Runs even after navigation/destruction; never dereferences the retired screen.
    if (r->client == g_store.client) {
        MailReply *draft = mail_reply_find(&g_store.mail_replies, completion->key, false);
        mail_reply_finish(draft, r->cancelled, r->ok, r->result, &r->error, (double)GetTickCount64());
        if (!r->ok && api_error_unauthorized(&r->error)) {
            r->auth_checked = true; store_invalidate_credentials(&r->error);
        }
    }
    free(completion->key); free(completion); r->completion_ctx = NULL;
}
static void reply_done(void *owner, Request *r) {
    Inbox *s = owner;
    if (!screen_mail_inbox_current(s, r)) return;
    screen_mail_inbox_repaint(s);
}

void screen_mail_reply_cancel(Inbox *s) {
    // Cancellation cannot undo provider delivery. The finalizer retains the interlock until transport ends.
    if (s->reply) {
        ReplyCompletion *completion = s->reply->completion_ctx;
        if (s->reply->client == g_store.client && completion) {
            MailReply *d = mail_reply_find(&g_store.mail_replies, completion->key, false);
            if (d) d->uncertain = true;
        }
        request_cancel(&s->reply);
    }
}

static char *reply_context(Inbox *s) {
    const MailAccount *a = mail_account_find(&s->accounts, s->body.account_id);
    const char *recipient = str_empty(s->body.reply_to) ? s->body.sender : s->body.reply_to;
    return xstrfmt("From %s\nTo %s\nThread: %s", a ? a->email : "", recipient ? recipient : "", screen_mail_inbox_subject_of(&s->body));
}

static void edit_reply(Inbox *s) {
    if (!screen_mail_reply_allowed(s)) return;
    MailReply *draft = screen_mail_reply_draft(s, true);
    if (!draft || draft->pending) return;
    char *key = xstrdup(draft->key), *text = xstrdup(draft->text), *context = reply_context(s);
    ApiClient *client = api_client_retain(g_store.client);
    int generation = s->generation;
    s->modal = true; screen_mail_inbox_cancel(s);
    bool saved = dialog_mail_reply(screen_mail_inbox_window(s), context, &text);
    free(context); s->modal = false;
    if (s->retired) {
        free(key); free(text); api_client_release(client); screen_mail_inbox_release(s); return;
    }
    bool current = s->shown && g_store.client == client && s->generation == generation;
    MailReply *selected = current ? screen_mail_reply_draft(s, false) : NULL;
    if (saved && selected && str_eq(key, selected->key) && screen_mail_reply_allowed(s)) {
        free(selected->text); selected->text = text; text = NULL; selected->open = true; selected->sent = false;
    }
    free(key); free(text); api_client_release(client);
    if (!current) screen_mail_inbox_clear_private(s);
    else { screen_mail_inbox_load_accounts(s); if (s->selected_id && !s->body.id) screen_mail_inbox_load_body(s); }
    screen_mail_inbox_arm(s); screen_mail_inbox_repaint(s);
}

static void send_reply(Inbox *s) {
    if (!screen_mail_reply_allowed(s) || s->reply) return;
    MailReply *draft = screen_mail_reply_draft(s, false);
    if (!draft || !draft->open || !mail_reply_begin(draft, (double)GetTickCount64())) return;
    Json *args = mail_reply_body(draft->text);
    json_set_num(args, "account", s->body.account_id); json_set_str(args, "id", s->body.id);
    ReplyCompletion *completion = xcalloc(1, sizeof *completion); completion->key = xstrdup(draft->key);
    Request *r = store_call("reply_mail_message", args, 0, s, reply_done, s->generation, &s->reply);
    r->completion_ctx = completion; r->completed = reply_completed;
    screen_mail_inbox_repaint(s);
}

static void reconcile_reply(Inbox *s) {
    MailReply *draft = screen_mail_reply_draft(s, false);
    if (!screen_mail_reply_allowed(s) || !draft || draft->pending || !draft->uncertain) return;
    char *key = xstrdup(draft->key);
    ApiClient *client = api_client_retain(g_store.client);
    s->modal = true; screen_mail_inbox_cancel(s);
    bool confirmed = app_confirm("Reconcile reply?", "Complete a fresh mailbox sync and check Sent at the provider first.\n\nOnly continue if you checked delivery and decided another send is appropriate. This does not resend mail.", "I synced and checked Sent", false);
    s->modal = false;
    if (s->retired) { free(key); api_client_release(client); screen_mail_inbox_release(s); return; }
    MailReply *selected = g_store.client == client ? screen_mail_reply_draft(s, false) : NULL;
    if (confirmed && selected && str_eq(key, selected->key) && screen_mail_reply_allowed(s)) mail_reply_reconcile(selected);
    free(key); api_client_release(client); screen_mail_inbox_load_accounts(s); screen_mail_inbox_arm(s); screen_mail_inbox_repaint(s);
}

void screen_mail_reply_action(Inbox *s, int act) {
    if (act == ACT_REPLY) edit_reply(s);
    else if (act == ACT_REPLY_SEND) send_reply(s);
    else if (act == ACT_REPLY_RECONCILE) reconcile_reply(s);
    else if (act == ACT_REPLY_CANCEL) {
        MailReply *draft = screen_mail_reply_draft(s, false);
        if (draft && !draft->pending) draft->open = false;
        screen_mail_inbox_repaint(s);
    }
}

void screen_mail_reply_layout(Inbox *s, Doc *doc, int x, int w) {
    if (!store_supports("reply_mail_message") || !mail_account_writable(&s->accounts, s->body.account_id)) return;
    MailReply *d = screen_mail_reply_draft(s, false);
    char *context = reply_context(s);
    doc_text(doc, x, w, context, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(context); doc_space(doc, px(8));
    bool ready = screen_mail_reply_allowed(s), pending = d && d->pending;
    if (d && d->sent) doc_notice(doc, x, w, "Reply accepted by the provider.");
    if (d && d->error) doc_notice(doc, x, w, d->error);
    if (pending) doc_loading(doc, x, w, "Sending reply...");
    if (d && d->uncertain) {
        doc_notice(doc, x, w, "Delivery is uncertain. Sync and check Sent before another send.");
        doc_button(doc, x, 0, "Reconcile after checking Sent", BUTTON_BORDERED, ACT_REPLY_RECONCILE, 0, ready && !pending);
    }
    doc_button(doc, x, 0, d && *d->text ? "Edit reply" : "Reply", BUTTON_BORDERED, ACT_REPLY, 0, ready && !pending);
    if (!d || !d->open) return;
    doc_space(doc, px(8));
    doc_text(doc, x, w, *d->text ? d->text : "Empty reply draft", FONT_BODY, theme.ink, DT_WORDBREAK);
    Json *body = mail_reply_body(d->text); bool valid = body != NULL; json_free(body);
    if (!valid) doc_notice(doc, x, w, "Use nonempty text, at most 100000 UTF-16 characters.");
    if ((double)GetTickCount64() < d->retry_until) doc_notice(doc, x, w, "Wait for the mail reply cooldown before sending again.");
    doc_button(doc, x, 0, "Send reply", BUTTON_PROMINENT, ACT_REPLY_SEND, 0,
        ready && !pending && !d->uncertain && valid && (double)GetTickCount64() >= d->retry_until);
    doc_button(doc, x, 0, "Cancel reply", BUTTON_BORDERED, ACT_REPLY_CANCEL, 0, !pending);
}
