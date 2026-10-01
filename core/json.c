#include "json.h"
#include "str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Json shared_null = { JSON_NULL, { .b = false } };

Json *json_null(void) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_NULL; return j; }
Json *json_bool(bool value) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_BOOL; j->b = value; return j; }
Json *json_number(double value) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_NUMBER; j->n = value; return j; }
Json *json_string(const char *value) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_STRING; j->s = xstrdup(value ? value : ""); return j; }
Json *json_string_or_null(const char *value) { return value ? json_string(value) : json_null(); }
Json *json_array(void) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_ARRAY; return j; }
Json *json_object(void) { Json *j = xcalloc(1, sizeof *j); j->type = JSON_OBJECT; return j; }

void json_free(Json *value) {
    if (!value || value == &shared_null) return;
    switch (value->type) {
    case JSON_STRING: free(value->s); break;
    case JSON_ARRAY:
        for (size_t i = 0; i < value->a.count; i++) json_free(value->a.items[i]);
        free(value->a.items); break;
    case JSON_OBJECT:
        for (size_t i = 0; i < value->o.count; i++) { free(value->o.keys[i]); json_free(value->o.vals[i]); }
        free(value->o.keys); free(value->o.vals); break;
    default: break;
    }
    free(value);
}

Json *json_clone(const Json *value) {
    if (!value) return json_null();
    switch (value->type) {
    case JSON_NULL: return json_null();
    case JSON_BOOL: return json_bool(value->b);
    case JSON_NUMBER: return json_number(value->n);
    case JSON_STRING: return json_string(value->s);
    case JSON_ARRAY: {
        Json *a = json_array();
        for (size_t i = 0; i < value->a.count; i++) json_array_push(a, json_clone(value->a.items[i]));
        return a;
    }
    case JSON_OBJECT: {
        Json *o = json_object();
        for (size_t i = 0; i < value->o.count; i++) json_object_set(o, value->o.keys[i], json_clone(value->o.vals[i]));
        return o;
    }
    }
    return json_null();
}

void json_array_push(Json *array, Json *value) {
    if (!array || array->type != JSON_ARRAY) { json_free(value); return; }
    if (array->a.count == array->a.cap) {
        array->a.cap = array->a.cap ? array->a.cap * 2 : 8;
        array->a.items = xrealloc(array->a.items, array->a.cap * sizeof *array->a.items);
    }
    array->a.items[array->a.count++] = value ? value : json_null();
}

static size_t object_index(const Json *object, const char *key) {
    for (size_t i = 0; i < object->o.count; i++) if (strcmp(object->o.keys[i], key) == 0) return i;
    return (size_t)-1;
}

void json_object_set(Json *object, const char *key, Json *value) {
    if (!object || object->type != JSON_OBJECT || !key) { json_free(value); return; }
    if (!value) value = json_null();
    size_t i = object_index(object, key);
    if (i != (size_t)-1) { json_free(object->o.vals[i]); object->o.vals[i] = value; return; }
    if (object->o.count == object->o.cap) {
        object->o.cap = object->o.cap ? object->o.cap * 2 : 8;
        object->o.keys = xrealloc(object->o.keys, object->o.cap * sizeof *object->o.keys);
        object->o.vals = xrealloc(object->o.vals, object->o.cap * sizeof *object->o.vals);
    }
    object->o.keys[object->o.count] = xstrdup(key);
    object->o.vals[object->o.count] = value;
    object->o.count++;
}

void json_object_remove(Json *object, const char *key) {
    if (!object || object->type != JSON_OBJECT || !key) return;
    size_t i = object_index(object, key);
    if (i == (size_t)-1) return;
    free(object->o.keys[i]); json_free(object->o.vals[i]);
    memmove(object->o.keys + i, object->o.keys + i + 1, (object->o.count - i - 1) * sizeof *object->o.keys);
    memmove(object->o.vals + i, object->o.vals + i + 1, (object->o.count - i - 1) * sizeof *object->o.vals);
    object->o.count--;
}

void json_object_merge(Json *into, const Json *from) {
    if (!from || from->type != JSON_OBJECT) return;
    for (size_t i = 0; i < from->o.count; i++) json_object_set(into, from->o.keys[i], json_clone(from->o.vals[i]));
}

const Json *json_get(const Json *object, const char *key) {
    if (!object || object->type != JSON_OBJECT || !key) return &shared_null;
    size_t i = object_index(object, key);
    return i == (size_t)-1 ? &shared_null : object->o.vals[i];
}
const Json *json_at(const Json *array, size_t index) {
    if (!array || array->type != JSON_ARRAY || index >= array->a.count) return &shared_null;
    return array->a.items[index];
}
size_t json_count(const Json *value) {
    if (!value) return 0;
    if (value->type == JSON_ARRAY) return value->a.count;
    if (value->type == JSON_OBJECT) return value->o.count;
    return 0;
}
const char *json_key(const Json *object, size_t index) {
    if (!object || object->type != JSON_OBJECT || index >= object->o.count) return NULL;
    return object->o.keys[index];
}

bool json_is_null(const Json *value) { return !value || value->type == JSON_NULL; }
bool json_is_object(const Json *value) { return value && value->type == JSON_OBJECT; }
bool json_is_array(const Json *value) { return value && value->type == JSON_ARRAY; }
const char *json_str(const Json *value) { return value && value->type == JSON_STRING ? value->s : NULL; }
const char *json_str_nonempty(const Json *value) { const char *s = json_str(value); return s && *s ? s : NULL; }
const char *json_str_or(const Json *value, const char *fallback) { const char *s = json_str(value); return s ? s : fallback; }
char *json_dup_str(const Json *value) { return xstrdup(json_str(value)); }
char **json_dup_strings(const Json *array, size_t *count) {
    size_t n = json_count(array), m = 0;
    char **out = xmalloc(n * sizeof *out);
    for (size_t i = 0; i < n; i++) { const char *s = json_str(json_at(array, i)); if (s) out[m++] = xstrdup(s); }
    *count = m;
    return out;
}
bool json_num(const Json *value, double *out) {
    if (!value || value->type != JSON_NUMBER) return false;
    if (out) *out = value->n;
    return true;
}
double json_num_or(const Json *value, double fallback) { double d; return json_num(value, &d) ? d : fallback; }
bool json_bool_is(const Json *value, bool expected) { return value && value->type == JSON_BOOL && value->b == expected; }
int json_bool_tristate(const Json *value) { return value && value->type == JSON_BOOL ? (value->b ? 1 : 0) : -1; }
bool json_is_set(const Json *value) {
    if (!value) return false;
    switch (value->type) {
    case JSON_NULL: return false;
    case JSON_BOOL: return value->b;
    case JSON_STRING: return value->s[0] != 0;
    case JSON_NUMBER: return value->n != 0;
    default: return true;
    }
}
int json_int_or(const Json *value, int fallback) {
    double d;
    if (!json_num(value, &d) || !isfinite(d) || d != floor(d) || d > 2147483647.0 || d < -2147483648.0) return fallback;
    return (int)d;
}
void json_set_str(Json *object, const char *key, const char *value) { if (value) json_object_set(object, key, json_string(value)); }
void json_set_num(Json *object, const char *key, double value) { json_object_set(object, key, json_number(value)); }
void json_set_bool(Json *object, const char *key, bool value) { json_object_set(object, key, json_bool(value)); }

bool json_equal(const Json *a, const Json *b) {
    if (!a) a = &shared_null;
    if (!b) b = &shared_null;
    if (a->type != b->type) return false;
    switch (a->type) {
    case JSON_NULL: return true;
    case JSON_BOOL: return a->b == b->b;
    case JSON_NUMBER: return a->n == b->n;
    case JSON_STRING: return strcmp(a->s, b->s) == 0;
    case JSON_ARRAY:
        if (a->a.count != b->a.count) return false;
        for (size_t i = 0; i < a->a.count; i++) if (!json_equal(a->a.items[i], b->a.items[i])) return false;
        return true;
    case JSON_OBJECT:
        if (a->o.count != b->o.count) return false;
        for (size_t i = 0; i < a->o.count; i++) {
            size_t j = object_index(b, a->o.keys[i]);
            if (j == (size_t)-1 || !json_equal(a->o.vals[i], b->o.vals[j])) return false;
        }
        return true;
    }
    return false;
}

// MARK: - Parser

typedef struct { const char *p, *end; int depth; } Parser;

static void skip_ws(Parser *ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}
static Json *parse_value(Parser *ps);

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool parse_hex4(Parser *ps, unsigned *out) {
    if (ps->end - ps->p < 4) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) { int d = hex_digit(ps->p[i]); if (d < 0) return false; v = v * 16 + (unsigned)d; }
    ps->p += 4; *out = v;
    return true;
}
static void append_utf8(Str *s, unsigned cp) {
    char buf[4]; int n = 0;
    if (cp < 0x80) { buf[n++] = (char)cp; }
    else if (cp < 0x800) { buf[n++] = (char)(0xC0 | (cp >> 6)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { buf[n++] = (char)(0xE0 | (cp >> 12)); buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { buf[n++] = (char)(0xF0 | (cp >> 18)); buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[n++] = (char)(0x80 | (cp & 0x3F)); }
    str_append(s, buf, (size_t)n);
}

static char *parse_string_raw(Parser *ps) {
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    Str s; str_init(&s);
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') return str_detach(&s);
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
            case '"': str_appendc(&s, '"'); break;
            case '\\': str_appendc(&s, '\\'); break;
            case '/': str_appendc(&s, '/'); break;
            case 'b': str_appendc(&s, '\b'); break;
            case 'f': str_appendc(&s, '\f'); break;
            case 'n': str_appendc(&s, '\n'); break;
            case 'r': str_appendc(&s, '\r'); break;
            case 't': str_appendc(&s, '\t'); break;
            case 'u': {
                unsigned cp;
                if (!parse_hex4(ps, &cp)) goto fail;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned low;
                    if (ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                        ps->p += 2;
                        if (!parse_hex4(ps, &low) || low < 0xDC00 || low > 0xDFFF) goto fail;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    } else cp = 0xFFFD;
                } else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0) cp = 0xFFFD;
                append_utf8(&s, cp);
                break;
            }
            default: goto fail;
            }
        } else if (c < 0x20) {
            goto fail;
        } else {
            str_appendc(&s, (char)c);
        }
    }
fail:
    str_free(&s);
    return NULL;
}

static Json *parse_number(Parser *ps) {
    const char *start = ps->p;
    if (ps->p < ps->end && *ps->p == '-') ps->p++;
    if (ps->p >= ps->end) return NULL;
    if (*ps->p == '0') ps->p++;
    else if (*ps->p >= '1' && *ps->p <= '9') { while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++; }
    else return NULL;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return NULL;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) ps->p++;
        if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') return NULL;
        while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') ps->p++;
    }
    char *tmp = xstrndup(start, (size_t)(ps->p - start));
    double d = strtod(tmp, NULL);
    free(tmp);
    if (!isfinite(d)) return NULL;
    return json_number(d);
}

static bool match_literal(Parser *ps, const char *lit) {
    size_t n = strlen(lit);
    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, lit, n) != 0) return false;
    ps->p += n;
    return true;
}

static Json *parse_value(Parser *ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    if (ps->depth > 512) return NULL;
    char c = *ps->p;
    if (c == '{') {
        ps->p++; ps->depth++;
        Json *o = json_object();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == '}') { ps->p++; ps->depth--; return o; }
        for (;;) {
            skip_ws(ps);
            char *key = parse_string_raw(ps);
            if (!key) { json_free(o); return NULL; }
            skip_ws(ps);
            if (ps->p >= ps->end || *ps->p != ':') { free(key); json_free(o); return NULL; }
            ps->p++;
            Json *v = parse_value(ps);
            if (!v) { free(key); json_free(o); return NULL; }
            json_object_set(o, key, v);
            free(key);
            skip_ws(ps);
            if (ps->p >= ps->end) { json_free(o); return NULL; }
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == '}') { ps->p++; ps->depth--; return o; }
            json_free(o); return NULL;
        }
    }
    if (c == '[') {
        ps->p++; ps->depth++;
        Json *a = json_array();
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ']') { ps->p++; ps->depth--; return a; }
        for (;;) {
            Json *v = parse_value(ps);
            if (!v) { json_free(a); return NULL; }
            json_array_push(a, v);
            skip_ws(ps);
            if (ps->p >= ps->end) { json_free(a); return NULL; }
            if (*ps->p == ',') { ps->p++; continue; }
            if (*ps->p == ']') { ps->p++; ps->depth--; return a; }
            json_free(a); return NULL;
        }
    }
    if (c == '"') { char *s = parse_string_raw(ps); if (!s) return NULL; Json *j = json_string(s); free(s); return j; }
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number(ps);
    if (match_literal(ps, "true")) return json_bool(true);
    if (match_literal(ps, "false")) return json_bool(false);
    if (match_literal(ps, "null")) return json_null();
    return NULL;
}

Json *json_parse(const char *text, size_t len) {
    if (!text) return NULL;
    // A UTF-8 byte order mark is not JSON but some proxies add one.
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) { text += 3; len -= 3; }
    Parser ps = { text, text + len, 0 };
    Json *v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) { json_free(v); return NULL; }
    return v;
}
Json *json_parsez(const char *text) { return text ? json_parse(text, strlen(text)) : NULL; }

// MARK: - Serializer

static void write_string(Str *out, const char *s) {
    str_appendc(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': str_appendz(out, "\\\""); break;
        case '\\': str_appendz(out, "\\\\"); break;
        case '\n': str_appendz(out, "\\n"); break;
        case '\r': str_appendz(out, "\\r"); break;
        case '\t': str_appendz(out, "\\t"); break;
        case '\b': str_appendz(out, "\\b"); break;
        case '\f': str_appendz(out, "\\f"); break;
        default:
            if (*p < 0x20) str_appendf(out, "\\u%04x", *p);
            else str_appendc(out, (char)*p);
        }
    }
    str_appendc(out, '"');
}

static void write_number(Str *out, double d) {
    if (!isfinite(d)) { str_appendz(out, "null"); return; }
    if (d == floor(d) && fabs(d) < 1e15) { str_appendf(out, "%lld", (long long)d); return; }
    char buf[64];
    snprintf(buf, sizeof buf, "%.17g", d);
    // Shortest representation that round-trips, as Foundation prints doubles.
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(buf, sizeof buf, "%.*g", precision, d);
        if (strtod(buf, NULL) == d) break;
    }
    str_appendz(out, buf);
}

static int compare_keys(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void indent(Str *out, int level) { for (int i = 0; i < level; i++) str_appendz(out, "  "); }

static void write_value(Str *out, const Json *v, bool sorted, int pretty_level) {
    bool pretty = pretty_level >= 0;
    if (!v) v = &shared_null;
    switch (v->type) {
    case JSON_NULL: str_appendz(out, "null"); break;
    case JSON_BOOL: str_appendz(out, v->b ? "true" : "false"); break;
    case JSON_NUMBER: write_number(out, v->n); break;
    case JSON_STRING: write_string(out, v->s); break;
    case JSON_ARRAY:
        str_appendc(out, '[');
        for (size_t i = 0; i < v->a.count; i++) {
            if (i) str_appendc(out, ',');
            if (pretty) { str_appendc(out, '\n'); indent(out, pretty_level + 1); }
            write_value(out, v->a.items[i], sorted, pretty ? pretty_level + 1 : -1);
        }
        if (pretty && v->a.count) { str_appendc(out, '\n'); indent(out, pretty_level); }
        str_appendc(out, ']');
        break;
    case JSON_OBJECT: {
        str_appendc(out, '{');
        size_t n = v->o.count;
        const char **keys = xmalloc((n ? n : 1) * sizeof *keys);
        for (size_t i = 0; i < n; i++) keys[i] = v->o.keys[i];
        if (sorted && n > 1) qsort(keys, n, sizeof *keys, compare_keys);
        for (size_t i = 0; i < n; i++) {
            if (i) str_appendc(out, ',');
            if (pretty) { str_appendc(out, '\n'); indent(out, pretty_level + 1); }
            write_string(out, keys[i]);
            str_appendz(out, pretty ? ": " : ":");
            write_value(out, v->o.vals[object_index(v, keys[i])], sorted, pretty ? pretty_level + 1 : -1);
        }
        free(keys);
        if (pretty && n) { str_appendc(out, '\n'); indent(out, pretty_level); }
        str_appendc(out, '}');
        break;
    }
    }
}

char *json_serialize(const Json *value, bool sorted) { Str s; str_init(&s); write_value(&s, value, sorted, -1); return str_detach(&s); }
char *json_pretty(const Json *value) { Str s; str_init(&s); write_value(&s, value, true, 0); return str_detach(&s); }
