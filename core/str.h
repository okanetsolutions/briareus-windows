// Small string helpers shared by the core and the app. UTF-8 everywhere; UTF-16 only at the Win32 edge.
#ifndef BRIAREUS_STR_H
#define BRIAREUS_STR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>
#include <wchar.h>

typedef struct { char *data; size_t len, cap; } Str;

void str_init(Str *s);
void str_free(Str *s);
void str_reserve(Str *s, size_t extra);
void str_append(Str *s, const char *bytes, size_t n);
void str_appendz(Str *s, const char *z);
void str_appendc(Str *s, char c);
void str_appendf(Str *s, const char *fmt, ...);
void str_vappendf(Str *s, const char *fmt, va_list ap);
/// Hands the buffer over as a NUL-terminated string and leaves the Str empty.
char *str_detach(Str *s);

void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *z);
char *xstrndup(const char *z, size_t n);
char *xstrfmt(const char *fmt, ...);

/// True for NULL and "".
bool str_empty(const char *z);
/// Strips ASCII whitespace at both ends into a new string.
char *str_trim(const char *z);
/// ASCII lowercase copy; NULL folds to "".
char *str_fold(const char *z);
bool str_eq(const char *a, const char *b);
bool str_ieq(const char *a, const char *b);
bool str_has_prefix(const char *z, const char *prefix);
bool str_has_suffix(const char *z, const char *suffix);
/// Case-insensitive substring test for ASCII.
bool str_icontains(const char *haystack, const char *needle);
char *str_replace(const char *z, const char *from, const char *to);
/// Capitalizes the first ASCII letter of a new copy.
char *str_capitalized(const char *z);
/// Splits on a separator into a NULL-terminated array of new strings; `*count` receives the number.
char **str_split(const char *z, char sep, size_t *count);
void str_array_free(char **items, size_t count);

wchar_t *utf8_to_wide(const char *z);
char *wide_to_utf8(const wchar_t *w);

#endif
