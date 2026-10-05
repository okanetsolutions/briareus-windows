#include "update.h"
#include "api.h"
#include "json.h"
#include "str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 0x00000800
#endif
#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_NEVER
#define WINHTTP_OPTION_REDIRECT_POLICY_NEVER 0
#endif

#define UPDATE_TIMEOUT_MS 120000
#define UPDATE_MAX_BYTES (64u * 1024 * 1024)
#define UPDATE_MAX_REDIRECTS 5

// MARK: - Releases

void update_release_free(UpdateRelease *r) {
    if (!r) return;
    free(r->version); free(r->page); free(r->url); free(r->sha256);
    memset(r, 0, sizeof *r);
}

static bool sha256_hex_valid(const char *hex) {
    if (!hex || strlen(hex) != 64) return false;
    for (const char *p = hex; *p; p++) if (!isxdigit((unsigned char)*p)) return false;
    return true;
}

bool update_release_parse(const char *text, size_t len, UpdateRelease *out) {
    memset(out, 0, sizeof *out);
    Json *json = json_parse(text, len);
    const char *tag = json_str_nonempty(json_get(json, "tag_name"));
    const Json *assets = json_get(json, "assets");
    bool ok = false;
    for (size_t i = 0; tag && i < json_count(assets); i++) {
        const Json *asset = json_at(assets, i);
        if (!str_eq(json_str(json_get(asset, "name")), UPDATE_ASSET)) continue;
        const char *url = json_str_nonempty(json_get(asset, "browser_download_url"));
        const char *digest = json_str(json_get(asset, "digest"));
        double size = json_num_or(json_get(asset, "size"), 0);
        if (!url || !str_has_prefix(digest, "sha256:") || !sha256_hex_valid(digest + 7) || size < 2 || size > UPDATE_MAX_BYTES) break;
        out->version = xstrdup(tag + (tag[0] == 'v' || tag[0] == 'V'));
        out->page = json_dup_str(json_get(json, "html_url"));
        out->url = xstrdup(url);
        out->sha256 = str_fold(digest + 7);
        out->size = (size_t)size;
        ok = true;
        break;
    }
    json_free(json);
    return ok;
}

/// Up to four dot-separated numbers after an optional `v`; false for anything else, a pre-release suffix included.
static bool version_parse(const char *z, long parts[4]) {
    memset(parts, 0, 4 * sizeof *parts);
    if (!z) return false;
    if (*z == 'v' || *z == 'V') z++;
    for (int i = 0; i < 4; i++) {
        if (!isdigit((unsigned char)*z)) return false;
        char *end; parts[i] = strtol(z, &end, 10); z = end;
        if (!*z) return true;
        if (*z++ != '.') return false;
    }
    return false;
}

bool update_newer(const char *candidate, const char *current) {
    long a[4], b[4];
    if (!version_parse(candidate, a) || !version_parse(current, b)) return false;
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

bool update_url_allowed(const char *url) {
    if (!url || _strnicmp(url, "https://", 8) != 0) return false;
    const char *host = url + 8;
    size_t len = strcspn(host, "/?#");
    // Credentials or a port in the address would let it name one host and reach another.
    if (!len || memchr(host, '@', len) || memchr(host, ':', len) || memchr(host, '\\', len)) return false;
    char *name = xstrndup(host, len), *folded = str_fold(name);
    free(name);
    static const char suffix[] = ".githubusercontent.com";
    size_t n = strlen(folded), s = sizeof suffix - 1;
    bool ok = str_eq(folded, "github.com") || str_eq(folded, "api.github.com") || (n > s && str_eq(folded + n - s, suffix));
    free(folded);
    return ok;
}

bool update_sha256(const void *data, size_t len, char hex[65]) {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool ok = false;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return false;
    if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
        const BYTE *p = data; size_t left = len;
        ok = true;
        while (ok && left) {
            DWORD chunk = left > 0x10000000 ? 0x10000000 : (DWORD)left;
            ok = CryptHashData(hash, p, chunk, 0) != 0;
            p += chunk; left -= chunk;
        }
        BYTE digest[32]; DWORD size = sizeof digest;
        if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof digest) {
            for (int i = 0; i < 32; i++) { static const char d[] = "0123456789abcdef"; hex[2 * i] = d[digest[i] >> 4]; hex[2 * i + 1] = d[digest[i] & 15]; }
            hex[64] = 0;
        } else ok = false;
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(provider, 0);
    return ok;
}

// MARK: - Network

bool update_winhttp_fetch(void *ctx, const char *url, const char *accept, int *status, char **location, char **body,
                          size_t *len, char **error) {
    (void)ctx;
    *status = 0; *location = NULL; *body = NULL; *len = 0; *error = NULL;
    HINTERNET session = NULL, connection = NULL, request = NULL;
    bool ok = false;
    wchar_t *wurl = utf8_to_wide(url), *waccept = NULL;
    size_t cap = wcslen(wurl) + 1;
    wchar_t *host = xcalloc(cap, sizeof *host), *path = xcalloc(cap, sizeof *path);
    URL_COMPONENTS parts; memset(&parts, 0, sizeof parts); parts.dwStructSize = sizeof parts;
    parts.lpszHostName = host; parts.dwHostNameLength = (DWORD)cap;
    parts.lpszUrlPath = path; parts.dwUrlPathLength = (DWORD)cap;
    if (!WinHttpCrackUrl(wurl, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) { *error = xstrdup("The update address is not an HTTPS URL."); goto done; }
    session = WinHttpOpen(L"Briareus-Windows/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { *error = api_winhttp_error_text(GetLastError()); goto done; }
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    if (!WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    connection = WinHttpConnect(session, host, parts.nPort, 0);
    if (connection) request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                 WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
    if (!request) { *error = api_winhttp_error_text(GetLastError()); goto done; }
    // Each redirect is followed by the caller, once its host is checked.
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof disabled);
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    WinHttpSetTimeouts(request, UPDATE_TIMEOUT_MS, UPDATE_TIMEOUT_MS, UPDATE_TIMEOUT_MS, UPDATE_TIMEOUT_MS);
    char *header = xstrfmt("Accept: %s\r\n", accept);
    waccept = utf8_to_wide(header);
    free(header);
    if (!WinHttpSendRequest(request, waccept, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request, NULL)) {
        *error = api_winhttp_error_text(GetLastError()); goto done;
    }
    DWORD code = 0, size = sizeof code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX)) {
        *error = api_winhttp_error_text(GetLastError()); goto done;
    }
    *status = (int)code;
    *location = api_winhttp_header(request, WINHTTP_QUERY_LOCATION);
    Str data; str_init(&data);
    for (;;) {
        DWORD available = 0, read = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) { *error = api_winhttp_error_text(GetLastError()); str_free(&data); goto done; }
        if (!available) break;
        str_reserve(&data, available);
        if (!WinHttpReadData(request, data.data + data.len, available, &read)) { *error = api_winhttp_error_text(GetLastError()); str_free(&data); goto done; }
        if (!read) break;
        data.len += read; data.data[data.len] = 0;
        if (data.len > UPDATE_MAX_BYTES) { *error = xstrdup("The download is too large."); str_free(&data); goto done; }
    }
    *len = data.len;
    *body = str_detach(&data);
    ok = true;
done:
    if (!ok) { free(*location); *location = NULL; }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    free(wurl); free(waccept); free(host); free(path);
    return ok;
}

static UpdateFetch g_fetch = update_winhttp_fetch;
static void *g_fetch_ctx;
void update_set_fetch(UpdateFetch fetch, void *ctx) { g_fetch = fetch ? fetch : update_winhttp_fetch; g_fetch_ctx = fetch ? ctx : NULL; }

/// A GET that follows redirects on the allowed hosts only and succeeds on a 2xx.
static bool fetch(const char *url, const char *accept, char **body, size_t *len, char **error) {
    char *current = xstrdup(url);
    *body = NULL; *len = 0; *error = NULL;
    for (int hop = 0; hop <= UPDATE_MAX_REDIRECTS; hop++) {
        if (!update_url_allowed(current)) { *error = xstrfmt("The update was redirected to an address it may not use: %s", current); break; }
        int status = 0; char *location = NULL, *response = NULL, *why = NULL; size_t got = 0;
        if (!g_fetch(g_fetch_ctx, current, accept, &status, &location, &response, &got, &why)) {
            *error = why ? why : xstrdup("GitHub could not be reached.");
            free(location); free(response);
            break;
        }
        free(why);
        if (status >= 300 && status < 400 && status != 304 && !str_empty(location)) {
            free(current); current = location; free(response);
            continue;
        }
        free(location);
        if (status >= 200 && status < 300) { *body = response; *len = got; free(current); return true; }
        free(response);
        *error = status == 403 || status == 429 ? xstrdup("GitHub is limiting update checks from this network. Try again later.")
               : status == 404 ? xstrdup("No release was found on GitHub.")
               : xstrfmt("GitHub answered the update request with HTTP %d.", status);
        break;
    }
    if (!*error) *error = xstrdup("The update was redirected too many times.");
    free(current);
    return false;
}

bool update_check(UpdateRelease *out, char **error) {
    memset(out, 0, sizeof *out);
    char *body; size_t len;
    if (!fetch(UPDATE_LATEST_URL, "application/vnd.github+json", &body, &len, error)) return false;
    bool ok = update_release_parse(body, len, out);
    free(body);
    if (!ok) *error = xstrdup("The latest release on GitHub has no Briareus.exe to update to.");
    return ok;
}

bool update_download(const UpdateRelease *r, char **bytes, size_t *len, char **error) {
    *bytes = NULL; *len = 0;
    char *body; size_t got;
    if (!fetch(r->url, "application/octet-stream", &body, &got, error)) return false;
    char hex[65];
    if (got != r->size || got < 2 || body[0] != 'M' || body[1] != 'Z' || !update_sha256(body, got, hex) || !str_eq(hex, r->sha256)) {
        free(body);
        *error = xstrdup("The downloaded update does not match the release on GitHub, so it was not installed.");
        return false;
    }
    *bytes = body; *len = got;
    return true;
}

// MARK: - Installing

static wchar_t *suffixed(const wchar_t *path, const wchar_t *suffix) {
    size_t n = wcslen(path) + wcslen(suffix) + 1;
    wchar_t *out = xmalloc(n * sizeof *out);
    wcscpy(out, path); wcscat(out, suffix);
    return out;
}
static char *install_error(const char *what) {
    char *why = api_winhttp_error_text(GetLastError());
    char *text = xstrfmt("%s (%s) Download it from the release page instead.", what, why);
    free(why);
    return text;
}

bool update_install(const wchar_t *exe, const void *bytes, size_t len, char **error) {
    *error = NULL;
    wchar_t *fresh = suffixed(exe, L".new"), *old = suffixed(exe, L".old");
    bool ok = false;
    HANDLE file = CreateFileW(fresh, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { *error = install_error("The update could not be saved beside Briareus.exe."); goto done; }
    const char *p = bytes; size_t left = len; bool written = true;
    while (written && left) {
        DWORD chunk = left > 0x100000 ? 0x100000 : (DWORD)left, wrote = 0;
        written = WriteFile(file, p, chunk, &wrote, NULL) && wrote == chunk;
        p += chunk; left -= chunk;
    }
    written = written && FlushFileBuffers(file);
    CloseHandle(file);
    if (!written) { *error = install_error("The update could not be saved beside Briareus.exe."); DeleteFileW(fresh); goto done; }
    DeleteFileW(old);
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) { *error = install_error("Briareus.exe could not be moved aside."); DeleteFileW(fresh); goto done; }
    if (!MoveFileExW(fresh, exe, 0)) {
        *error = install_error("The update could not be moved in.");
        MoveFileExW(old, exe, 0);
        DeleteFileW(fresh);
        goto done;
    }
    ok = true;
done:
    free(fresh); free(old);
    return ok;
}

bool update_cleanup(const wchar_t *exe) {
    wchar_t *old = suffixed(exe, L".old");
    bool ok = DeleteFileW(old) || GetLastError() == ERROR_FILE_NOT_FOUND;
    free(old);
    return ok;
}
