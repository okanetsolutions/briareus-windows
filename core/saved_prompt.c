#include "saved_prompt.h"
#include "str.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

bool saved_prompt_scope_allowed(const char *permission, char *const *repos, size_t count, const char *repo) {
    if (str_eq(permission, "admin")) return true;
    if (!str_eq(permission, "read") && !str_eq(permission, "manage")) return false;
    if (!repo) return false;
    for (size_t i = 0; i < count; i++) if (str_eq(repo, repos[i])) return true;
    return false;
}
static bool space(unsigned c) {
    if (c >= 9 && c <= 13) return true;
    if (c >= 0x2000 && c <= 0x200a) return true;
    return c == 32 || c == 0xa0 || c == 0x1680 || c == 0x2028 || c == 0x2029
        || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
static bool decode(const unsigned char **p, unsigned *out) {
    unsigned c = *(*p)++, min = 0; int extra = 0;
    if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; min = 0x10000; }
    else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; min = 0x800; }
    else if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; min = 0x80; }
    else if (c >= 0x80) return false;
    for (int i = 0; i < extra; i++) {
        if ((**p & 0xc0) != 0x80) return false;
        c = (c << 6) | (*(*p)++ & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    *out = c; return true;
}
// Server String.trim for title; body removes surrounding blank lines and trailing
// whitespace, retaining indentation before its first nonblank line (savedprompts.js).
static char *normalized(const char *text, bool body, size_t max) {
    if (!text) return NULL;
    const unsigned char *p = (const unsigned char *)text;
    size_t start = 0, end = 0, units = 0, kept_units = 0; bool leading = true;
    while (*p) {
        unsigned c; if (!decode(&p, &c)) return NULL;
        size_t offset = (size_t)((const char *)p - text);
        if (leading && space(c)) {
            if (!body || c == '\n') start = offset;
        } else {
            leading = false; units += c > 0xffff ? 2 : 1;
            if (!space(c)) { end = offset; kept_units = units; }
        }
    }
    // Leading body indentation counts toward the JS length too.
    if (body && end > start) {
        p = (const unsigned char *)text + start; kept_units = 0;
        while ((size_t)((const char *)p - text) < end) { unsigned c; if (!decode(&p, &c)) return NULL; kept_units += c > 0xffff ? 2 : 1; }
    }
    if (!end || kept_units > max) return NULL;
    return xstrndup(text + start, end - start);
}
static bool field(Json *out, const Json *args, const char *key, bool body, size_t max, bool required) {
    const Json *v = json_get(args, key);
    bool present = false;
    for (size_t i = 0; i < json_count(args); i++) if (str_eq(json_key(args, i), key)) present = true;
    if (!present && !required) return true;
    char *value = normalized(json_str(v), body, max);
    if (!value) return false;
    json_set_str(out, key, value); free(value); return true;
}
Json *saved_prompt_wire(const Json *args, bool create) {
    Json *out = json_object();
    bool valid = field(out, args, "title", false, 120, create) && field(out, args, "body", true, 20000, create);
    if (!valid) { json_free(out); return NULL; }
    if (create) {
        const Json *repo = json_get(args, "repo");
        if (!json_is_null(repo) && !json_str(repo)) { json_free(out); return NULL; }
        if (json_str_nonempty(repo)) json_set_str(out, "repo", json_str(repo));
    } else {
        int id = json_int_or(json_get(args, "id"), 0);
        if (id <= 0 || !json_count(out)) { json_free(out); return NULL; }
        json_set_num(out, "id", id);
    }
    return out;
}
Json *saved_prompt_create(const char *title, const char *body, const char *repo) {
    Json *args = json_object(); json_set_str(args, "title", title); json_set_str(args, "body", body); json_set_str(args, "repo", repo);
    Json *out = saved_prompt_wire(args, true); json_free(args); return out;
}
Json *saved_prompt_edit(const Json *original, const char *title, const char *body) {
    Json *out = saved_prompt_create(title, body, NULL);
    if (!out || json_int_or(json_get(original, "id"), 0) <= 0) { json_free(out); return NULL; }
    if (str_eq(json_str(json_get(out, "title")), json_str(json_get(original, "title")))) json_object_remove(out, "title");
    if (str_eq(json_str(json_get(out, "body")), json_str(json_get(original, "body")))) json_object_remove(out, "body");
    if (json_count(out)) json_object_set(out, "id", json_clone(json_get(original, "id")));
    return out;
}
static int compare(const void *a, const void *b) {
    const Json *x = *(const Json *const *)a, *y = *(const Json *const *)b;
    double xo = json_num_or(json_get(x, "sortOrder"), INFINITY), yo = json_num_or(json_get(y, "sortOrder"), INFINITY);
    if (xo < yo) return -1;
    if (xo > yo) return 1;
    return strcmp(json_str(json_get(x, "title")), json_str(json_get(y, "title")));
}
Json *saved_prompt_list(const Json *answer, const char *repo) {
    const Json *rows = json_get(answer, "prompts"); if (!json_is_array(rows)) return NULL;
    Json *out = json_array();
    for (size_t i = 0; i < json_count(rows); i++) {
        const Json *p = json_at(rows, i);
        if (!json_str_nonempty(json_get(p, "title")) || !json_str(json_get(p, "body"))) continue;
        const char *scope = json_str_nonempty(json_get(p, "repo"));
        if (repo && scope && !str_eq(repo, scope)) continue;
        json_array_push(out, json_clone(p));
    }
    if (json_count(out) > 1) qsort(out->a.items, out->a.count, sizeof *out->a.items, compare);
    return out;
}
const Json *saved_prompt_find(const Json *rows, int id) {
    if (id <= 0) return NULL;
    for (size_t i = 0; i < json_count(rows); i++) if (json_int_or(json_get(json_at(rows, i), "id"), 0) == id) return json_at(rows, i);
    return NULL;
}
char *saved_prompt_insert(const char *draft, const char *body) {
    char *trimmed = normalized(draft ? draft : "", false, (size_t)-1);
    char *out = trimmed ? xstrfmt("%s\n\n%s", draft, body ? body : "") : xstrdup(body ? body : "");
    free(trimmed); return out;
}
