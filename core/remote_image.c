// Pictures linked from an agent's reply: which links are pictures, where they come from, and their download.
#include "remote_image.h"
#include "str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define REMOTE_IMAGE_TIMEOUT_MS 30000

static const char *authority_end(const char *authority) {
    const char *end = authority;
    while (*end && *end != '/' && *end != '?' && *end != '#') end++;
    return end;
}
static const char *path_end(const char *path) {
    const char *end = path;
    while (*end && *end != '?' && *end != '#') end++;
    return end;
}

bool remote_image_address(const char *url) {
    if (!url || _strnicmp(url, "https://", 8) != 0) return false;
    const char *authority = url + 8, *end = authority_end(authority);
    // The host ends at a path or at the end, so nothing after it can be taken for the host by another reader of the
    // address ("https://a?@b/"), and it is spelled with nothing but what a host name, a port or an IPv6 literal has.
    if (end == authority || *authority == ':' || (*end && *end != '/')) return false;
    for (const char *p = authority; p < end; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("-._:[]", *p)) return false;
    }
    for (const char *p = end; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 32 || c == 127 || c == '\\') return false;
    }
    return true;
}

bool remote_image_url(const char *url) {
    if (!remote_image_address(url)) return false;
    const char *path = authority_end(url + 8), *end = path_end(path), *dot = NULL;
    for (const char *p = path; p < end; p++) { if (*p == '/') dot = NULL; else if (*p == '.') dot = p; }
    if (!dot) return false;
    static const char *const extensions[] = { ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp" };
    for (size_t i = 0; i < sizeof extensions / sizeof *extensions; i++) {
        size_t n = strlen(extensions[i]);
        if ((size_t)(end - dot) == n && _strnicmp(dot, extensions[i], n) == 0) return true;
    }
    return false;
}

char *remote_image_host(const char *url) {
    if (!remote_image_address(url)) return NULL;
    const char *authority = url + 8;
    char *host = xstrndup(authority, (size_t)(authority_end(authority) - authority));
    for (char *p = host; *p; p++) *p = (char)tolower((unsigned char)*p);
    size_t n = strlen(host);
    if (n > 4 && str_eq(host + n - 4, ":443")) host[n - 4] = 0;
    return host;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

char *remote_image_name(const char *url) {
    char *host = remote_image_host(url);
    if (!host) return xstrdup("");
    const char *path = authority_end(url + 8), *end = path_end(path), *start = path;
    for (const char *p = path; p < end; p++) if (*p == '/') start = p + 1;
    Str name; str_init(&name);
    for (const char *p = start; p < end; p++) {
        int high = *p == '%' && end - p > 2 ? hex_digit(p[1]) : -1, low = high >= 0 ? hex_digit(p[2]) : -1;
        // A decoded control character or separator stays out of what is shown and offered as a file name.
        int code = low >= 0 ? high * 16 + low : (unsigned char)*p;
        if (low >= 0) p += 2;
        str_appendc(&name, code < 32 || code == 127 || code == '/' || code == '\\' ? '_' : (char)code);
    }
    if (!name.len) { str_free(&name); return host; }
    free(host);
    return str_detach(&name);
}

bool remote_image_from_origin(const char *url, const char *origin) {
    if (!origin || !str_has_prefix(origin, "https://")) return false;
    char *host = remote_image_host(url);
    bool same = host && str_eq(host, origin + 8);
    free(host);
    return same;
}

bool remote_image_recognized(const void *bytes, size_t len) {
    const unsigned char *b = bytes;
    if (!b || len < 12) return false;
    if (memcmp(b, "\x89PNG\r\n\x1A\n", 8) == 0) return true;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return true;
    if (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0) return true;
    if (memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0) return true;
    return b[0] == 'B' && b[1] == 'M';
}

static ApiTransport g_transport = api_winhttp_transport;
static void *g_transport_ctx;
void remote_image_set_transport(ApiTransport transport, void *ctx) {
    g_transport = transport ? transport : api_winhttp_transport;
    g_transport_ctx = transport ? ctx : NULL;
}

/// Why a picture that was answered is not taken, or NULL when it is.
static char *refusal(int status, const char *body, size_t len) {
    if (status >= 300 && status < 400) return xstrdup("The picture's address redirects elsewhere, which is not followed.");
    if (status != 200) return xstrfmt("The server answered %d for the picture.", status);
    if (len > REMOTE_IMAGE_MAX) return xstrdup("The picture is larger than 32 MB.");
    if (!remote_image_recognized(body, len)) return xstrdup("The link does not hold a PNG, JPEG, GIF, BMP or WebP picture.");
    return NULL;
}

bool remote_image_download(const char *url, char **bytes, size_t *len, char **error) {
    *bytes = NULL; *len = 0; *error = NULL;
    if (!remote_image_address(url)) { *error = xstrdup("The picture's address is not an HTTPS link."); return false; }
    // The token never goes with it: the address is whatever the reply named.
    static const char *const headers[] = { "Accept", "image/png, image/jpeg, image/gif, image/bmp, image/webp, */*;q=0.5", NULL };
    const char *hash = strchr(url, '#');
    char *address = hash ? xstrndup(url, (size_t)(hash - url)) : xstrdup(url);
    int status = 0; char *type = NULL, *retry = NULL, *body = NULL, *why = NULL; size_t n = 0;
    bool received = g_transport(g_transport_ctx, "GET", address, headers, NULL, 0, REMOTE_IMAGE_TIMEOUT_MS,
                                &status, &type, &retry, &body, &n, &why);
    free(address); free(type); free(retry);
    if (!received) { *error = why ? why : xstrdup("The picture's server could not be reached."); free(body); return false; }
    free(why);
    *error = refusal(status, body, n);
    if (*error) { free(body); return false; }
    *bytes = body; *len = n;
    return true;
}
