#ifndef BRIAREUS_MAIL_REPLY_H
#define BRIAREUS_MAIL_REPLY_H
#include "mail.h"
#include "api.h"

// Owned connection-lifetime, memory-only drafts. No message bodies or credentials are persisted.
typedef struct MailReply {
    char *key, *text;
    const char *error; // borrowed static message
    bool pending, uncertain, sent, open;
    double retry_until;
    struct MailReply *next;
} MailReply;
bool mail_reply_same_account(const MailAccount *before, const MailAccount *after);
bool mail_account_writable(const MailAccounts *accounts, int id);
/// Owned stable identity: account, provider, address, creation timestamp and original message id.
char *mail_reply_key(const MailAccounts *accounts, const MailMessage *message);
/// Borrowed draft; creates it only when requested. key is copied.
MailReply *mail_reply_find(MailReply **drafts, const char *key, bool create);
void mail_replies_free(MailReply **drafts);
/// NULL for blank/invalid UTF-8 or over 100000 UTF-16 units; preserves the original text.
Json *mail_reply_body(const char *text);
bool mail_reply_begin(MailReply *draft, double now);
void mail_reply_finish(MailReply *draft, bool cancelled, bool ok, const Json *answer, const ApiError *error, double now);
/// Explicit operator acknowledgement after fresh sync and checking Sent, never while transport is pending.
bool mail_reply_reconcile(MailReply *draft);
#endif
