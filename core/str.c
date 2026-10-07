#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

static void oom(void) { fputs("out of memory\n", stderr); abort(); }
void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) oom(); return p; }
void *xcalloc(size_t count, size_t size) { void *p = calloc(count ? count : 1, size ? size : 1); if (!p) oom(); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) oom(); return p; }
void *xmalloc_kept(size_t n) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // A client block, which the harness does not count as leaked. The debug heap asserts if free() releases it.
    void *p = _malloc_dbg(n ? n : 1, _CLIENT_BLOCK, __FILE__, __LINE__); if (!p) oom(); return p;
#else
    return xmalloc(n);
#endif
}
void xfree_kept(void *p) {
#if defined(_MSC_VER) && defined(_DEBUG)
    _free_dbg(p, _CLIENT_BLOCK);
#else
    free(p);
#endif
}
char *xstrdup(const char *z) { if (!z) return NULL; size_t n = strlen(z); char *c = xmalloc(n + 1); memcpy(c, z, n + 1); return c; }
char *xstrndup(const char *z, size_t n) { char *c = xmalloc(n + 1); memcpy(c, z, n); c[n] = 0; return c; }
char *xstrfmt(const char *fmt, ...) {
    Str s; str_init(&s);
    va_list ap; va_start(ap, fmt); str_vappendf(&s, fmt, ap); va_end(ap);
    return str_detach(&s);
}

void str_init(Str *s) { s->data = NULL; s->len = s->cap = 0; }
void str_free(Str *s) { free(s->data); str_init(s); }
void str_reserve(Str *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 64;
    while (cap < s->len + extra + 1) cap *= 2;
    s->data = xrealloc(s->data, cap); s->cap = cap;
}
void str_append(Str *s, const char *bytes, size_t n) {
    str_reserve(s, n); memcpy(s->data + s->len, bytes, n); s->len += n; s->data[s->len] = 0;
}
void str_appendz(Str *s, const char *z) { if (z) str_append(s, z, strlen(z)); }
void str_appendc(Str *s, char c) { str_append(s, &c, 1); }
void str_vappendf(Str *s, const char *fmt, va_list ap) {
    va_list copy; va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy); va_end(copy);
    if (n < 0) return;
    str_reserve(s, (size_t)n);
    vsnprintf(s->data + s->len, (size_t)n + 1, fmt, ap);
    s->len += (size_t)n;
}
void str_appendf(Str *s, const char *fmt, ...) { va_list ap; va_start(ap, fmt); str_vappendf(s, fmt, ap); va_end(ap); }
char *str_detach(Str *s) {
    char *out = s->data ? s->data : xstrdup("");
    str_init(s);
    return out;
}

bool str_empty(const char *z) { return !z || !*z; }
static bool is_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
char *str_trim(const char *z) {
    if (!z) return xstrdup("");
    while (is_space((unsigned char)*z)) z++;
    size_t n = strlen(z);
    while (n > 0 && is_space((unsigned char)z[n - 1])) n--;
    return xstrndup(z, n);
}
char *str_fold(const char *z) {
    char *c = xstrdup(z ? z : "");
    for (char *p = c; *p; p++) *p = (char)tolower((unsigned char)*p);
    return c;
}
bool str_eq(const char *a, const char *b) { if (!a || !b) return a == b; return strcmp(a, b) == 0; }
bool str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    return *a == *b;
}
bool str_has_prefix(const char *z, const char *prefix) { return z && prefix && strncmp(z, prefix, strlen(prefix)) == 0; }
bool str_has_suffix(const char *z, const char *suffix) {
    if (!z || !suffix) return false;
    size_t n = strlen(z), m = strlen(suffix);
    return m <= n && memcmp(z + n - m, suffix, m) == 0;
}
bool str_icontains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    if (!*needle) return true;
    size_t m = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < m && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == m) return true;
    }
    return false;
}
char *str_replace(const char *z, const char *from, const char *to) {
    Str s; str_init(&s);
    size_t m = strlen(from);
    if (!m) { str_appendz(&s, z); return str_detach(&s); }
    for (const char *p = z; *p;) {
        if (strncmp(p, from, m) == 0) { str_appendz(&s, to); p += m; }
        else str_appendc(&s, *p++);
    }
    return str_detach(&s);
}
char *str_capitalized(const char *z) {
    char *c = xstrdup(z ? z : "");
    if (*c) *c = (char)toupper((unsigned char)*c);
    return c;
}
char **str_split(const char *z, char sep, size_t *count) {
    size_t n = 0, cap = 8;
    char **items = xmalloc(cap * sizeof *items);
    const char *start = z ? z : "";
    for (const char *p = start;; p++) {
        if (*p == sep || !*p) {
            if (n + 1 >= cap) { cap *= 2; items = xrealloc(items, cap * sizeof *items); }
            items[n++] = xstrndup(start, (size_t)(p - start));
            start = p + 1;
            if (!*p) break;
        }
    }
    items[n] = NULL;
    *count = n;
    return items;
}
void str_array_free(char **items, size_t count) {
    if (!items) return;
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
}

wchar_t *utf8_to_wide(const char *z) {
    if (!z) z = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, z, -1, NULL, 0);
    if (n <= 0) n = 1;
    wchar_t *w = xmalloc((size_t)n * sizeof *w);
    if (MultiByteToWideChar(CP_UTF8, 0, z, -1, w, n) <= 0) w[0] = 0;
    return w;
}
char *wide_to_utf8(const wchar_t *w) {
    if (!w) w = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) n = 1;
    char *z = xmalloc((size_t)n);
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, z, n, NULL, NULL) <= 0) z[0] = 0;
    return z;
}
