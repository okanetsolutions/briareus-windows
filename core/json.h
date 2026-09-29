// Extensible server payloads preserve unknown fields without tying the app to every dashboard release.
#ifndef BRIAREUS_JSON_H
#define BRIAREUS_JSON_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT } JsonType;

typedef struct Json Json;
struct Json {
    JsonType type;
    union {
        bool b;
        double n;
        char *s;
        struct { Json **items; size_t count, cap; } a;
        struct { char **keys; Json **vals; size_t count, cap; } o;
    };
};

/// Parses one document; NULL when the text is not JSON. Trailing whitespace is allowed, anything else is not.
Json *json_parse(const char *text, size_t len);
Json *json_parsez(const char *text);

Json *json_null(void);
Json *json_bool(bool value);
Json *json_number(double value);
Json *json_string(const char *value);
Json *json_array(void);
Json *json_object(void);
Json *json_clone(const Json *value);
void json_free(Json *value);

/// Both take ownership of `value`. Setting a key that exists replaces its value.
void json_array_push(Json *array, Json *value);
void json_object_set(Json *object, const char *key, Json *value);
void json_object_remove(Json *object, const char *key);
/// Copies every key of `from` into `into`, replacing what was there.
void json_object_merge(Json *into, const Json *from);

/// Subscripts never fail: a missing key, or a non-object, reads as a shared immutable null.
const Json *json_get(const Json *object, const char *key);
const Json *json_at(const Json *array, size_t index);
size_t json_count(const Json *array_or_object);
/// The i-th key of an object, or NULL past the end.
const char *json_key(const Json *object, size_t index);

bool json_is_null(const Json *value);
bool json_is_object(const Json *value);
bool json_is_array(const Json *value);
/// The string, or NULL for anything else.
const char *json_str(const Json *value);
/// The string when it is not empty, or NULL.
const char *json_str_nonempty(const Json *value);
/// True with the number in `*out` when the value is one.
bool json_num(const Json *value, double *out);
double json_num_or(const Json *value, double fallback);
/// True only for the boolean `expected`.
bool json_bool_is(const Json *value, bool expected);
/// -1 when the value is not a boolean, else 0 or 1.
int json_bool_tristate(const Json *value);
/// JavaScript truthiness for flags the server sends as a value or leaves null.
bool json_is_set(const Json *value);
bool json_equal(const Json *a, const Json *b);

/// Compact serialization. `sorted` orders object keys so equal values are equal bytes.
char *json_serialize(const Json *value, bool sorted);
/// Indented, with sorted keys, for reading unknown payloads.
char *json_pretty(const Json *value);

/// A number that is a whole number within int range, or `fallback`.
int json_int_or(const Json *value, int fallback);
/// Convenience: `json_object_set(obj, key, json_string(value))` when `value` is not NULL.
void json_set_str(Json *object, const char *key, const char *value);
void json_set_num(Json *object, const char *key, double value);
void json_set_bool(Json *object, const char *key, bool value);

#endif
