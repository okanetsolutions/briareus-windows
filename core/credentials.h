// The device token lives in Windows Credential Manager, scoped by canonical server origin; only the origin is saved in the registry.
#ifndef BRIAREUS_CREDENTIALS_H
#define BRIAREUS_CREDENTIALS_H
#include <stdbool.h>

/// The saved token for an origin, or NULL when there is none. `*failed` is set when the store itself could not be read.
char *credentials_read(const char *origin, bool *failed);
bool credentials_save(const char *token, const char *origin);
bool credentials_remove(const char *origin);
/// What a failed store access should say.
const char *credentials_failure_text(void);

char *settings_read_origin(void);
void settings_write_origin(const char *origin);
void settings_remove_origin(void);
/// The user's own GitHub login, which "Assign me" adds; the API does not say whose the token is. NULL until saved.
char *settings_read_github_login(void);
void settings_write_github_login(const char *login);
/// The runtime last picked for a new session (runtime_choice_saved), so the next one starts on it. NULL until picked.
char *settings_read_last_runtime(void);
void settings_write_last_runtime(const char *saved);
/// Whether new releases are downloaded and installed on their own; on until turned off.
bool settings_read_auto_update(void);
void settings_write_auto_update(bool on);

#endif
