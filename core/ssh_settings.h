#ifndef BRIAREUS_SSH_SETTINGS_H
#define BRIAREUS_SSH_SETTINGS_H
#include "json.h"

enum { SSH_PASSWORD, SSH_SUDO_PASSWORD, SSH_SECRET_COUNT };
typedef struct {
    bool offered, stored, remove;
    char *text; size_t bytes;
    unsigned long long revision;
} SshSecret;
typedef struct { SshSecret fields[SSH_SECRET_COUNT]; } SshSecrets;
extern const char *const SSH_SECRET_KEYS[SSH_SECRET_COUNT];
extern const char *const SSH_SECRET_FLAGS[SSH_SECRET_COUNT];
/// Owns draft buffers. New forms infer each capability from rows; only a known empty list assumes both.
void ssh_secrets_init(SshSecrets *s, const Json *row, const Json *rows, bool fresh);
void ssh_secrets_free(SshSecrets *s);
void ssh_secrets_set(SshSecrets *s, int field, const char *text, size_t bytes);
void ssh_secrets_toggle(SshSecrets *s, int field);
bool ssh_secrets_changed(const SshSecrets *s);
/// Strips read-only flags and includes only offered changes. Diagnostic never contains values.
bool ssh_secrets_body(const SshSecrets *s, Json *body, char **error);
/// Saved flags come only from the response. Only acknowledged drafts at the sent revision are cleared.
bool ssh_secrets_saved(SshSecrets *s, const Json *row, const Json *sent, const unsigned long long revisions[SSH_SECRET_COUNT]);
/// Clone carries capabilities and typed drafts, never stored flags or removal requests. Destination is empty.
void ssh_secrets_clone(SshSecrets *copy, const SshSecrets *s);
/// Erases an owned temporary text buffer before freeing it.
void ssh_secret_free(char *text, size_t bytes);
/// Erases password values in a transient JSON object before its normal release.
void ssh_secrets_scrub(Json *body);
#endif
