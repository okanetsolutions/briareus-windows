// Mail account settings only; message reading builds on these public, credential-free models.
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
#endif
