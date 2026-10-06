#include "credentials.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincred.h>

static wchar_t *target_for(const char *origin) {
    char *name = xstrfmt("Briareus device token:%s", origin ? origin : "");
    wchar_t *w = utf8_to_wide(name);
    free(name);
    return w;
}

char *credentials_read(const char *origin, bool *failed) {
    if (failed) *failed = false;
    wchar_t *target = target_for(origin);
    PCREDENTIALW cred = NULL;
    BOOL ok = CredReadW(target, CRED_TYPE_GENERIC, 0, &cred);
    free(target);
    if (!ok) {
        DWORD code = GetLastError();
        if (code != ERROR_NOT_FOUND && failed) *failed = true;
        return NULL;
    }
    char *token = NULL;
    if (cred->CredentialBlob && cred->CredentialBlobSize) token = xstrndup((const char *)cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return token;
}

bool credentials_save(const char *token, const char *origin) {
    wchar_t *target = target_for(origin);
    CREDENTIALW cred; memset(&cred, 0, sizeof cred);
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target;
    cred.CredentialBlobSize = (DWORD)strlen(token);
    cred.CredentialBlob = (LPBYTE)token;
    // This device only: the token never roams with the account.
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.UserName = (LPWSTR)L"device";
    cred.Comment = (LPWSTR)L"Briareus client API token";
    BOOL ok = CredWriteW(&cred, 0);
    free(target);
    return ok != 0;
}

bool credentials_remove(const char *origin) {
    wchar_t *target = target_for(origin);
    BOOL ok = CredDeleteW(target, CRED_TYPE_GENERIC, 0);
    DWORD code = ok ? 0 : GetLastError();
    free(target);
    return ok || code == ERROR_NOT_FOUND;
}

const char *credentials_failure_text(void) {
    return "Could not access the device token in Credential Manager. Sign in to Windows again and try again.";
}

#define SETTINGS_KEY L"Software\\Okanet\\Briareus"

static char *settings_read(const wchar_t *name) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return NULL;
    wchar_t buffer[1024]; DWORD size = sizeof buffer, type = 0;
    LSTATUS status = RegQueryValueExW(key, name, NULL, &type, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return NULL;
    buffer[size / sizeof(wchar_t) < 1023 ? size / sizeof(wchar_t) : 1023] = 0;
    char *value = wide_to_utf8(buffer);
    if (!*value) { free(value); return NULL; }
    return value;
}
static void settings_write(const wchar_t *name, const char *value) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    wchar_t *w = utf8_to_wide(value);
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)w, (DWORD)((wcslen(w) + 1) * sizeof(wchar_t)));
    free(w);
    RegCloseKey(key);
}

char *settings_read_origin(void) { return settings_read(L"serverOrigin"); }

void settings_write_origin(const char *origin) { settings_write(L"serverOrigin", origin); }

void settings_remove_origin(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return;
    RegDeleteValueW(key, L"serverOrigin");
    RegCloseKey(key);
}

char *settings_read_github_login(void) { return settings_read(L"githubLogin"); }
void settings_write_github_login(const char *login) { settings_write(L"githubLogin", login); }
char *settings_read_last_runtime(void) { return settings_read(L"lastRuntime"); }
void settings_write_last_runtime(const char *saved) { settings_write(L"lastRuntime", saved); }
bool settings_read_auto_update(void) {
    char *value = settings_read(L"autoUpdate");
    bool on = !str_eq(value, "off");
    free(value);
    return on;
}
void settings_write_auto_update(bool on) { settings_write(L"autoUpdate", on ? "on" : "off"); }
