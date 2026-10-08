// Credential-free mail settings and memory-only message models.
#ifndef BRIAREUS_MAIL_H
#define BRIAREUS_MAIL_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    int id, sync_days, messages, unread;
    char *provider, *email, *label, *status, *last_sync_error;
    bool enabled, syncing;
    double last_sync_at, created_at, updated_at; // epoch milliseconds; zero when absent
} MailAccount;
typedef struct {
    MailAccount *accounts; size_t count;
    bool gmail, outlook;
    char *default_label;
    bool default_enabled;
    int default_sync_days;
} MailAccounts;
bool mail_account_parse(const Json *value, MailAccount *out);
void mail_account_free(MailAccount *account);
bool mail_accounts_parse(const Json *value, MailAccounts *out);
void mail_accounts_free(MailAccounts *accounts);
const MailAccount *mail_account_find(const MailAccounts *accounts, int id);
bool mail_provider_available(const MailAccounts *accounts, const char *provider);
/// Validates a settings-only body; never copies credentials or other response fields.
Json *mail_settings_body(const char *label, bool enabled, const char *days);

typedef struct {
    char *state, *redirect_uri, *provider;
    double expires_at;
    int account_id;
    bool server_finish;
    MailAccounts before;
} MailSignIn;
bool mail_sign_in_parse(const Json *value, const char *provider, int account_id, const MailAccounts *before, MailSignIn *out);
void mail_sign_in_free(MailSignIn *sign_in);
/// Validates redirect destination, single state/code, matching state and expiry before sending an exchange.
Json *mail_sign_in_finish(const MailSignIn *sign_in, const char *url, double now_ms);
/// No updatedAt heuristic: normal syncs update that timestamp too.
bool mail_sign_in_completed(const MailSignIn *sign_in, const MailAccounts *current);
/// Safe user-facing errors; only recognized validation details affect the message.
const char *mail_error_message(int status, bool finishing, const char *detail);

typedef struct { char *id, *name, *mime_type; double size; } MailAttachment;
typedef struct {
    int account_id;
    char *id, *thread_id, *sender, *to, *cc, *reply_to, *subject, *snippet, *web_url, *text;
    char **labels; size_t label_count;
    MailAttachment *attachments; size_t attachment_count;
    double received_at;
    bool is_read, in_inbox, is_starred, truncated;
} MailMessage;
typedef struct { MailMessage *messages; size_t count; char *next_cursor; } MailMessages;
void mail_message_free(MailMessage *message);
/// Never retains body.html; summaries never retain any body.
bool mail_message_parse(const Json *value, bool body, MailMessage *out);
void mail_messages_free(MailMessages *messages);
bool mail_messages_parse(const Json *value, MailMessages *out);
const MailMessage *mail_message_find(const MailMessages *messages, int account, const char *id);
/// Transfers a page, deduplicating by account+id and sorting newest first.
void mail_messages_append(MailMessages *messages, MailMessages *page);
bool mail_account_readable(const MailAccounts *accounts, int id);
/// Purges removed/revoked accounts; true if private state changed.
bool mail_messages_prune(MailMessages *messages, const MailAccounts *accounts);
bool mail_message_web_url_safe(const char *url);
typedef struct { int account, unread, inbox, starred; char *q, *label, *thread; } MailFilter;
void mail_filter_init(MailFilter *filter);
void mail_filter_free(MailFilter *filter);
/// Flags: -1 any, 0 false, 1 true; omit empty filters and first-page cursor.
Json *mail_filter_args(const MailFilter *filter, const char *cursor);
const char *mail_read_error(int status, bool detail);
#endif
