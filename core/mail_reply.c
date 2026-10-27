#include "mail_reply.h"
#include "str.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

bool mail_reply_same_account(const MailAccount *before, const MailAccount *after) {
    if (!before || !after) return false;
    return before->id == after->id && before->created_at == after->created_at
        && str_eq(before->provider, after->provider) && str_eq(before->email, after->email);
}

bool mail_account_writable(const MailAccounts *accounts, int id) {
    const MailAccount *a = mail_account_find(accounts, id);
    if (!a || !str_eq(a->status, "connected") || a->read_only) return false;
    if (!mail_provider_available(accounts, a->provider)) return false;
    return !a->access || !*a->access || str_eq(a->access, "manage");
}

char *mail_reply_key(const MailAccounts *accounts, const MailMessage *m) {
    const MailAccount *a = m ? mail_account_find(accounts, m->account_id) : NULL;
    if (!a || str_empty(m->id)) return NULL;
    return xstrfmt("%d:%.0f:%lu:%s:%lu:%s:%s", a->id, a->created_at,
        (unsigned long)strlen(a->provider), a->provider, (unsigned long)strlen(a->email), a->email, m->id);
}

MailReply *mail_reply_find(MailReply **drafts, const char *key, bool create) {
    if (!key) return NULL;
    for (MailReply *d = *drafts; d; d = d->next) if (str_eq(d->key, key)) return d;
    if (!create) return NULL;
    MailReply *d = xcalloc(1, sizeof *d); d->key = xstrdup(key); d->text = xstrdup("");
    d->next = *drafts; *drafts = d; return d;
}

void mail_replies_free(MailReply **drafts) {
    while (*drafts) {
        MailReply *d = *drafts; *drafts = d->next;
        free(d->key); free(d->text); free(d);
    }
}

static bool js_space(unsigned c) {
    if (c >= 9 && c <= 13) return true;
    if (c >= 0x2000 && c <= 0x200a) return true;
    return c == 32 || c == 0xa0 || c == 0x1680 || c == 0x2028 || c == 0x2029
        || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

Json *mail_reply_body(const char *text) {
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    size_t units = 0; bool nonempty = false;
    while (*p) {
        unsigned c = *p++, min = 0; int extra = 0;
        if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; min = 0x10000; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; min = 0x800; }
        else if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; min = 0x80; }
        else if (c >= 0x80) return NULL;
        for (int i = 0; i < extra; i++) {
            if ((*p & 0xc0) != 0x80) return NULL;
            c = (c << 6) | (*p++ & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return NULL;
        units += c > 0xffff ? 2 : 1;
        if (units > 100000) return NULL;
        if (!js_space(c)) nonempty = true;
    }
    if (!nonempty) return NULL;
    Json *body = json_object(); json_set_str(body, "text", text); return body;
}

bool mail_reply_begin(MailReply *d, double now) {
    if (!d || d->pending || d->uncertain || now < d->retry_until) return false;
    Json *body = mail_reply_body(d->text);
    if (!body) return false;
    json_free(body); d->pending = true; d->sent = false; d->error = NULL; return true;
}

static const char *reply_error(int status) {
    switch (status) {
    case 400: return "The server refused the reply. Use nonempty text within 100000 characters.";
    case 401: return "This token expired or was revoked. Reconnect to the server.";
    case 403: return "Reconnect this mailbox in Settings > Mail with manage access; an Admin server token is also required.";
    case 404: return "The message or mailbox is no longer available. Refresh mail.";
    case 409: return "The mailbox changed or needs sign-in. Refresh its status in Settings > Mail.";
    case 429: return "Mail is rate limited. Wait for the cooldown before sending again.";
    default: return "The reply may have been sent. Sync the mailbox and check Sent before confirming reconciliation here.";
    }
}

void mail_reply_finish(MailReply *d, bool cancelled, bool ok, const Json *answer, const ApiError *e, double now) {
    if (!d || !d->pending) return;
    d->pending = false;
    if (e && isfinite(e->retry_after) && e->retry_after > 0) {
        double until = now + e->retry_after * 1000;
        if (!isfinite(until)) until = DBL_MAX;
        if (until > d->retry_until) d->retry_until = until;
    }
    if (e && e->status == 429 && d->retry_until < now + 10000) d->retry_until = now + 10000;
    bool accepted = ok && str_eq(json_str(json_get(answer, "status")), "accepted")
        && str_eq(json_str(json_get(answer, "action")), "reply");
    if (!cancelled && accepted) {
        free(d->text); d->text = xstrdup(""); d->sent = true; d->open = false; d->error = NULL; return;
    }
    d->uncertain = cancelled || ok || !api_error_is_refusal(e) || (e && e->status == 408);
    d->error = reply_error(d->uncertain ? 0 : e->status);
}

bool mail_reply_reconcile(MailReply *d) {
    if (!d || d->pending || !d->uncertain) return false;
    d->uncertain = false; d->error = NULL; return true;
}
