#ifndef BRIAREUS_MAIL_WAKE_H
#define BRIAREUS_MAIL_WAKE_H
#include "mail.h"

enum { MAIL_WAKE_LIMIT = 20, MAIL_WAKE_CAPS = 2, MAIL_WAKE_FILTERS = 3 };
extern const char *const mail_wake_cap_keys[MAIL_WAKE_CAPS];
extern const char *const mail_wake_filter_keys[MAIL_WAKE_FILTERS];
extern const char *const mail_wake_cap_labels[MAIL_WAKE_CAPS];
extern const char *const mail_wake_filter_labels[MAIL_WAKE_FILTERS];
// Owned editable rules and caps plus their baseline. Zero-init and free with mail_wake_free.
typedef struct { Json *rules, *saved_rules; char *caps[MAIL_WAKE_CAPS], *saved_caps[MAIL_WAKE_CAPS]; } MailWake;
void mail_wake_free(MailWake *wake);
// Atomic read: malformed records leave the previous draft intact. Missing fields alone use defaults.
bool mail_wake_read(MailWake *wake, const Json *record);
bool mail_wake_dirty(const MailWake *wake);
void mail_wake_discard(MailWake *wake);
bool mail_wake_active(const Json *record);
bool mail_wake_mailbox_usable(const MailAccounts *accounts, int id);
// -1 unknown/malformed/missing project; 0 off; 1 on. Only pass GET /settings/projects.
int mail_wake_tools(const Json *projects, const char *repo);
bool mail_wake_add(MailWake *wake, int account);
void mail_wake_remove(MailWake *wake, size_t row);
// Owned body or NULL and an owned explanation; never copies server bookkeeping into a PUT.
bool mail_wake_saved_matches(const MailWake *saved, const Json *sent);
Json *mail_wake_body(const MailWake *wake, const MailAccounts *accounts, int tools, char **error);
#endif
