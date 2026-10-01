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

char *settings_read_origin(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS) return NULL;
    wchar_t buffer[1024]; DWORD size = sizeof buffer, type = 0;
    LSTATUS status = RegQueryValueExW(key, L"serverOrigin", NULL, &type, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return NULL;
    buffer[size / sizeof(wchar_t) < 1023 ? size / sizeof(wchar_t) : 1023] = 0;
    char *origin = wide_to_utf8(buffer);
    if (!*origin) { free(origin); return NULL; }
    return origin;
}

void settings_write_origin(const char *origin) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL) != ERROR_SUCCESS) return;
    wchar_t *w = utf8_to_wide(origin);
    RegSetValueExW(key, L"serverOrigin", 0, REG_SZ, (const BYTE *)w, (DWORD)((wcslen(w) + 1) * sizeof(wchar_t)));
    free(w);
    RegCloseKey(key);
}

void settings_remove_origin(void) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS) return;
    RegDeleteValueW(key, L"serverOrigin");
    RegCloseKey(key);
}
