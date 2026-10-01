// json.c: parsing, building, accessors, equality and serialization.
#include "json.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool parses(const char *text) { Json *j = json_parsez(text); bool ok = j != NULL; json_free(j); return ok; }

/// Parses and serializes compactly with sorted keys; NULL when the text is rejected.
static char *roundtrip(const char *text) {
    Json *j = json_parsez(text);
    if (!j) return NULL;
    char *out = json_serialize(j, true);
    json_free(j);
    return out;
}

static double parsed_number(const char *text) {
    Json *j = json_parsez(text);
    double d = json_num_or(j, NAN);
    json_free(j);
    return d;
}

static void test_parses_scalars(void) {
    Json *j = json_parsez("null"); CHECK(j && j->type == JSON_NULL); json_free(j);
    j = json_parsez("true"); CHECK(j && j->type == JSON_BOOL && j->b); json_free(j);
    j = json_parsez("false"); CHECK(j && j->type == JSON_BOOL && !j->b); json_free(j);
    j = json_parsez("\"hi\""); CHECK_STR(json_str(j), "hi"); json_free(j);
    j = json_parsez("12"); CHECK(j && j->type == JSON_NUMBER && j->n == 12); json_free(j);
}

static void test_parses_nested_containers(void) {
    Json *j = json_parsez(" { \"a\" : [ 1 , { \"b\" : null } , [ ] , { } ] , \"c\" : \"d\" } ");
    CHECK(json_is_object(j));
    CHECK_INT(json_count(j), 2);
    CHECK_STR(json_key(j, 0), "a"); CHECK_STR(json_key(j, 1), "c");
    const Json *a = json_get(j, "a");
    CHECK(json_is_array(a)); CHECK_INT(json_count(a), 4);
    CHECK_INT(json_int_or(json_at(a, 0), -1), 1);
    CHECK(json_is_object(json_at(a, 1)));
    CHECK(json_get(json_at(a, 1), "b")->type == JSON_NULL);
    CHECK_INT(json_count(json_at(a, 1)), 1);
    CHECK(json_is_array(json_at(a, 2))); CHECK_INT(json_count(json_at(a, 2)), 0);
    CHECK(json_is_object(json_at(a, 3))); CHECK_INT(json_count(json_at(a, 3)), 0);
    CHECK_STR(json_str(json_get(j, "c")), "d");
    json_free(j);
}

static void test_whitespace_around_the_document_is_allowed(void) {
    CHECK(parses(" \t\r\n[1]\n\t "));
    CHECK(parses("\n{}\n"));
    // Form feed is not JSON whitespace.
    CHECK(!parses("\f1"));
    CHECK(!parses("1\f"));
}

static void test_trailing_garbage_is_rejected(void) {
    CHECK(!parses("{} x"));
    CHECK(!parses("[1] [2]"));
    CHECK(!parses("1 2"));
    CHECK(!parses("truex"));
    CHECK(!parses("nullnull"));
    CHECK(!parses("\"a\"\"b\""));
    CHECK(!parses("{},"));
}

static void test_malformed_documents_are_rejected(void) {
    const char *bad[] = { "", "   ", "{", "}", "[", "]", "[1,]", "[,1]", "[1 2]", "{\"a\":1,}", "{\"a\"}", "{\"a\" 1}",
                          "{a:1}", "{'a':1}", "{1:2}", "{\"a\":}", "['x']", "tru", "nul", "fals", "True", "NULL",
                          "undefined", "NaN", "Infinity", "-Infinity", "\"unterminated", "[\"a\"", "{\"a\":1", "[1,",
                          "{\"a\":1 \"b\":2}", "/* c */ 1", "// c\n1" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        bool ok = parses(bad[i]);
        CHECK(!ok);
        if (ok) printf("  accepted: %s\n", bad[i]);
    }
}

static void test_null_text_is_rejected(void) {
    CHECK(json_parsez(NULL) == NULL);
    CHECK(json_parse(NULL, 4) == NULL);
    CHECK(json_parse("1", 0) == NULL);
}

static void test_parse_honours_the_length(void) {
    Json *j = json_parse("[1,2]garbage", 5);
    CHECK(json_is_array(j)); CHECK_INT(json_count(j), 2);
    json_free(j);
    j = json_parse("123456", 3);
    CHECK_INT(json_int_or(j, -1), 123);
    json_free(j);
    j = json_parse("truex", 4);
    CHECK(json_bool_is(j, true));
    json_free(j);
    CHECK(json_parse("\"abc\"", 4) == NULL);
    CHECK(json_parse("tru", 3) == NULL);
    CHECK(json_parse("-", 1) == NULL);
    CHECK(json_parse("1.", 2) == NULL);
    CHECK(json_parse("1e", 2) == NULL);
    CHECK(json_parse("\"a\\", 3) == NULL);
    CHECK(json_parse("\"\\u12", 5) == NULL);
}

static void test_an_embedded_nul_inside_the_length_is_rejected(void) {
    CHECK(json_parse("[1]\0", 4) == NULL);
    CHECK(json_parse("\"a\0b\"", 5) == NULL);
}

static void test_a_utf8_byte_order_mark_is_skipped(void) {
    Json *j = json_parsez("\xef\xbb\xbf{\"a\":1}");
    CHECK(json_is_object(j)); CHECK_INT(json_int_or(json_get(j, "a"), 0), 1);
    json_free(j);
    j = json_parsez("\xef\xbb\xbf  [] ");
    CHECK(json_is_array(j));
    json_free(j);
    CHECK(!parses("\xef\xbb\xbf"));
    CHECK(!parses("\xef\xbb"));
    CHECK(!parses(" \xef\xbb\xbf{}"));
    CHECK(!parses("\xef\xbb\xbf\xef\xbb\xbf{}"));
}

static void test_numbers_in_every_form(void) {
    CHECK(parsed_number("0") == 0);
    CHECK(parsed_number("-0") == 0);
    CHECK(parsed_number("7") == 7);
    CHECK(parsed_number("-42") == -42);
    CHECK(parsed_number("3.25") == 3.25);
    CHECK(parsed_number("-0.5") == -0.5);
    CHECK(parsed_number("1e3") == 1000);
    CHECK(parsed_number("1E3") == 1000);
    CHECK(parsed_number("1e+3") == 1000);
    CHECK(parsed_number("25e-2") == 0.25);
    CHECK(parsed_number("-1.5E-1") == -0.15);
    CHECK(parsed_number("0e0") == 0);
    CHECK(parsed_number("1e400") != parsed_number("1e400"));
    CHECK(parsed_number("1e-400") == 0);
    CHECK(parsed_number("9007199254740993") == 9007199254740992.0);
    CHECK(parsed_number("123456789012345678901234567890") > 1.2e29);
}

static void test_malformed_numbers_are_rejected(void) {
    const char *bad[] = { "01", "-01", "00", "+1", ".5", "-.5", "1.", "1.e3", "1e", "1e+", "1e-", "-", "--1", "0x10",
                          "1.2.3", "1e2e3", "1e400", "-1e400", "[1e999]", "- 1", "1_000", "\xd9\xa1" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        bool ok = parses(bad[i]);
        CHECK(!ok);
        if (ok) printf("  accepted: %s\n", bad[i]);
    }
}

static void test_string_escapes(void) {
    Json *j = json_parsez("\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t\"");
    CHECK_STR(json_str(j), "q\" b\\ s/ \b\f\n\r\t");
    json_free(j);
    j = json_parsez("\"\\u0041\\u00e9\\u20AC\\u20ac\"");
    CHECK_STR(json_str(j), "A\xc3\xa9\xe2\x82\xac\xe2\x82\xac");
    json_free(j);
    j = json_parsez("\"\\u007f\\u0080\\u07ff\\u0800\\uffff\"");
    CHECK_STR(json_str(j), "\x7f\xc2\x80\xdf\xbf\xe0\xa0\x80\xef\xbf\xbf");
    json_free(j);
}

static void test_raw_utf8_passes_through_strings(void) {
    Json *j = json_parsez("\"caf\xc3\xa9 \xf0\x9f\x98\x80 \xe4\xb8\xad\"");
    CHECK_STR(json_str(j), "caf\xc3\xa9 \xf0\x9f\x98\x80 \xe4\xb8\xad");
    json_free(j);
    j = json_parsez("{\"cl\xc3\xa9\":1}");
    CHECK_INT(json_int_or(json_get(j, "cl\xc3\xa9"), 0), 1);
    json_free(j);
}

static void test_surrogate_pairs_decode_to_one_code_point(void) {
    Json *j = json_parsez("\"\\ud83d\\ude00\"");
    CHECK_STR(json_str(j), "\xf0\x9f\x98\x80");
    json_free(j);
    j = json_parsez("\"\\uD834\\uDD1E!\"");
    CHECK_STR(json_str(j), "\xf0\x9d\x84\x9e!");
    json_free(j);
    j = json_parsez("\"\\udbff\\udfff\"");
    CHECK_STR(json_str(j), "\xf4\x8f\xbf\xbf");
    json_free(j);
}

static void test_lone_surrogates_become_the_replacement_character(void) {
    Json *j = json_parsez("\"a\\ud83db\"");
    CHECK_STR(json_str(j), "a\xef\xbf\xbd" "b");
    json_free(j);
    j = json_parsez("\"\\ude00\"");
    CHECK_STR(json_str(j), "\xef\xbf\xbd");
    json_free(j);
    j = json_parsez("\"\\ud83d\"");
    CHECK_STR(json_str(j), "\xef\xbf\xbd");
    json_free(j);
    j = json_parsez("\"\\ud83d\\n\"");
    CHECK_STR(json_str(j), "\xef\xbf\xbd\n");
    json_free(j);
    // A high surrogate followed by a \u that is not a low surrogate is rejected outright.
    CHECK(!parses("\"\\ud83d\\u0041\""));
    CHECK(!parses("\"\\ud83d\\ud83d\""));
}

static void test_bad_escapes_and_control_characters_are_rejected(void) {
    const char *bad[] = { "\"\\x41\"", "\"\\a\"", "\"\\'\"", "\"\\u12\"", "\"\\u12G4\"", "\"\\uzzzz\"", "\"\\U0041\"",
                          "\"tab\there\"", "\"line\nbreak\"", "\"\x01\"", "\"\x1f\"", "\"\\\"", "\"\\ud83d\\uZZZZ\"" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        bool ok = parses(bad[i]);
        CHECK(!ok);
        if (ok) printf("  accepted: %s\n", bad[i]);
    }
    CHECK(parses("\"\x7f\""));
}

static void test_duplicate_keys_keep_the_last_value(void) {
    Json *j = json_parsez("{\"a\":1,\"b\":2,\"a\":3}");
    CHECK_INT(json_count(j), 2);
    CHECK_INT(json_int_or(json_get(j, "a"), 0), 3);
    CHECK_STR(json_key(j, 0), "a");
    json_free(j);
}

static void test_deep_nesting_is_bounded(void) {
    char *ok = xmalloc(201), *deep = xmalloc(200001);
    memset(ok, '[', 100); memset(ok + 100, ']', 100); ok[200] = 0;
    CHECK(parses(ok));
    memset(deep, '[', 100000); memset(deep + 100000, ']', 100000); deep[200000] = 0;
    CHECK(!parses(deep));
    memset(deep, '[', 100000); deep[100000] = 0;
    CHECK(!parses(deep));
    free(ok); free(deep);
}

static void test_nesting_limit_boundary(void) {
    // 512 nested containers with a value inside still parse; 600 do not.
    Str s; str_init(&s);
    for (int i = 0; i < 512; i++) str_appendc(&s, '[');
    str_appendc(&s, '1');
    for (int i = 0; i < 512; i++) str_appendc(&s, ']');
    CHECK(parses(s.data));
    str_free(&s);
    for (int i = 0; i < 600; i++) str_appendc(&s, '[');
    str_appendc(&s, '1');
    for (int i = 0; i < 600; i++) str_appendc(&s, ']');
    CHECK(!parses(s.data));
    str_free(&s);
    for (int i = 0; i < 600; i++) str_appendz(&s, "{\"k\":");
    str_appendz(&s, "1");
    for (int i = 0; i < 600; i++) str_appendc(&s, '}');
    CHECK(!parses(s.data));
    str_free(&s);
}

static void test_large_arrays_and_objects_parse(void) {
    Str s; str_init(&s);
    str_appendc(&s, '[');
    for (int i = 0; i < 5000; i++) str_appendf(&s, i ? ",%d" : "%d", i);
    str_appendc(&s, ']');
    Json *j = json_parsez(s.data);
    CHECK_INT(json_count(j), 5000);
    CHECK_INT(json_int_or(json_at(j, 4999), -1), 4999);
    char *back = json_serialize(j, false);
    CHECK_STR(back, s.data);
    free(back);
    json_free(j);
    str_free(&s);

    str_appendc(&s, '{');
    for (int i = 0; i < 500; i++) str_appendf(&s, i ? ",\"k%d\":%d" : "\"k%d\":%d", i, i);
    str_appendc(&s, '}');
    j = json_parsez(s.data);
    CHECK_INT(json_count(j), 500);
    CHECK_INT(json_int_or(json_get(j, "k321"), -1), 321);
    CHECK_STR(json_key(j, 499), "k499");
    json_free(j);
    str_free(&s);
}

static void test_constructors_make_each_type(void) {
    Json *n = json_null(), *t = json_bool(true), *f = json_bool(false), *num = json_number(2.5), *s = json_string("x");
    Json *s0 = json_string(NULL), *on = json_string_or_null(NULL), *os = json_string_or_null("y"), *a = json_array(), *o = json_object();
    CHECK(n->type == JSON_NULL);
    CHECK(t->type == JSON_BOOL && t->b); CHECK(f->type == JSON_BOOL && !f->b);
    CHECK(num->type == JSON_NUMBER && num->n == 2.5);
    CHECK_STR(json_str(s), "x");
    CHECK_STR(json_str(s0), "");
    CHECK(on->type == JSON_NULL);
    CHECK_STR(json_str(os), "y");
    CHECK(json_is_array(a) && json_count(a) == 0);
    CHECK(json_is_object(o) && json_count(o) == 0);
    json_free(n); json_free(t); json_free(f); json_free(num); json_free(s); json_free(s0); json_free(on); json_free(os); json_free(a); json_free(o);
    json_free(NULL);
}

static void test_json_string_copies_its_input(void) {
    char buf[] = "abc";
    Json *s = json_string(buf);
    buf[0] = 'z';
    CHECK_STR(json_str(s), "abc");
    json_free(s);
}

static void test_array_push_takes_ownership_and_grows(void) {
    Json *a = json_array();
    for (int i = 0; i < 100; i++) json_array_push(a, json_number(i));
    json_array_push(a, NULL);
    CHECK_INT(json_count(a), 101);
    CHECK_INT(json_int_or(json_at(a, 0), -1), 0);
    CHECK_INT(json_int_or(json_at(a, 99), -1), 99);
    CHECK(json_at(a, 100)->type == JSON_NULL);
    CHECK(json_at(a, 100) != json_at(a, 101));
    json_free(a);
    // Pushing onto a non-array frees the value instead of leaking it.
    Json *o = json_object();
    json_array_push(o, json_string("dropped"));
    CHECK_INT(json_count(o), 0);
    json_array_push(NULL, json_string("dropped"));
    json_free(o);
}

static void test_object_set_keeps_insertion_order_and_replaces(void) {
    Json *o = json_object();
    json_object_set(o, "z", json_number(1));
    json_object_set(o, "a", json_number(2));
    json_object_set(o, "m", json_number(3));
    json_object_set(o, "a", json_string("replaced"));
    CHECK_INT(json_count(o), 3);
    CHECK_STR(json_key(o, 0), "z"); CHECK_STR(json_key(o, 1), "a"); CHECK_STR(json_key(o, 2), "m");
    CHECK(json_key(o, 3) == NULL);
    CHECK_STR(json_str(json_get(o, "a")), "replaced");
    json_object_set(o, "n", NULL);
    CHECK_INT(json_count(o), 4);
    CHECK(json_get(o, "n")->type == JSON_NULL);
    json_object_set(o, NULL, json_number(9));
    CHECK_INT(json_count(o), 4);
    Json *a = json_array();
    json_object_set(a, "k", json_number(1));
    CHECK_INT(json_count(a), 0);
    json_object_set(NULL, "k", json_number(1));
    for (int i = 0; i < 50; i++) { char key[8]; snprintf(key, sizeof key, "k%d", i); json_object_set(o, key, json_number(i)); }
    CHECK_INT(json_count(o), 54);
    CHECK_INT(json_int_or(json_get(o, "k49"), -1), 49);
    json_free(a);
    json_free(o);
}

static void test_object_set_copies_the_key(void) {
    char key[] = "name";
    Json *o = json_object();
    json_object_set(o, key, json_number(1));
    key[0] = 'g';
    CHECK_STR(json_key(o, 0), "name");
    CHECK_INT(json_int_or(json_get(o, "name"), 0), 1);
    json_free(o);
}

static void test_object_remove(void) {
    Json *o = json_parsez("{\"a\":1,\"b\":[2],\"c\":3}");
    json_object_remove(o, "b");
    CHECK_INT(json_count(o), 2);
    CHECK_STR(json_key(o, 0), "a"); CHECK_STR(json_key(o, 1), "c");
    CHECK(json_is_null(json_get(o, "b")));
    json_object_remove(o, "missing");
    CHECK_INT(json_count(o), 2);
    json_object_remove(o, "c");
    json_object_remove(o, "a");
    CHECK_INT(json_count(o), 0);
    json_object_remove(o, "a");
    json_object_set(o, "x", json_bool(true));
    CHECK_INT(json_count(o), 1);
    json_free(o);
    Json *a = json_parsez("[1]");
    json_object_remove(a, "0");
    CHECK_INT(json_count(a), 1);
    json_object_remove(NULL, "a");
    json_free(a);
}

static void test_object_merge_replaces_and_adds(void) {
    Json *into = json_parsez("{\"a\":1,\"b\":{\"x\":1},\"c\":3}");
    Json *from = json_parsez("{\"b\":{\"y\":2},\"d\":[4],\"a\":null}");
    json_object_merge(into, from);
    char *out = json_serialize(into, false);
    CHECK_STR(out, "{\"a\":null,\"b\":{\"y\":2},\"c\":3,\"d\":[4]}");
    free(out);
    // The merged values are copies, so freeing the source leaves them intact.
    json_free(from);
    CHECK_INT(json_int_or(json_get(json_get(into, "b"), "y"), 0), 2);
    Json *arr = json_parsez("[1]");
    json_object_merge(into, arr);
    json_object_merge(into, NULL);
    CHECK_INT(json_count(into), 4);
    Json *target = json_array();
    Json *src = json_parsez("{\"k\":1}");
    json_object_merge(target, src);
    CHECK_INT(json_count(target), 0);
    json_free(target); json_free(src); json_free(arr); json_free(into);
}

static void test_clone_is_deep_and_equal(void) {
    Json *src = json_parsez("{\"s\":\"t\",\"n\":1.5,\"b\":false,\"z\":null,\"a\":[1,[2,{\"k\":\"v\"}]],\"o\":{}}");
    Json *copy = json_clone(src);
    CHECK(copy != src);
    CHECK(json_equal(src, copy));
    char *a = json_serialize(src, false), *b = json_serialize(copy, false);
    CHECK_STR(a, b);
    free(a); free(b);
    json_object_set((Json *)json_at(json_at(json_get(copy, "a"), 1), 1), "k", json_string("changed"));
    CHECK_STR(json_str(json_get(json_at(json_at(json_get(src, "a"), 1), 1), "k")), "v");
    CHECK(!json_equal(src, copy));
    json_free(src); json_free(copy);
    Json *n = json_clone(NULL);
    CHECK(n && n->type == JSON_NULL);
    json_free(n);
    // Cloning the shared null yields a value the caller owns.
    Json *empty = json_object();
    Json *missing = json_clone(json_get(empty, "x"));
    CHECK(missing->type == JSON_NULL && missing != json_get(empty, "x"));
    json_free(missing); json_free(empty);
}

static void test_subscripts_never_fail(void) {
    Json *o = json_parsez("{\"a\":[1],\"s\":\"x\"}");
    const Json *shared = json_get(o, "missing");
    CHECK(shared != NULL && shared->type == JSON_NULL);
    CHECK(json_get(o, NULL) == shared);
    CHECK(json_get(NULL, "a") == shared);
    CHECK(json_get(json_get(o, "a"), "a") == shared);
    CHECK(json_get(json_get(o, "s"), "x") == shared);
    CHECK(json_get(json_get(json_get(o, "x"), "y"), "z") == shared);
    CHECK(json_at(o, 0) == shared);
    CHECK(json_at(json_get(o, "a"), 1) == shared);
    CHECK(json_at(json_get(o, "a"), (size_t)-1) == shared);
    CHECK(json_at(NULL, 0) == shared);
    CHECK_INT(json_int_or(json_at(json_get(o, "a"), 0), 0), 1);
    // Freeing the shared null is a no-op, so code that frees what it got back stays safe.
    json_free((Json *)shared);
    CHECK(json_get(o, "missing")->type == JSON_NULL);
    json_free(o);
}

static void test_count_and_key_on_every_type(void) {
    Json *o = json_parsez("{\"a\":1}"), *a = json_parsez("[1,2,3]"), *s = json_string("abc"), *n = json_number(3);
    CHECK_INT(json_count(o), 1);
    CHECK_INT(json_count(a), 3);
    CHECK_INT(json_count(s), 0);
    CHECK_INT(json_count(n), 0);
    CHECK_INT(json_count(NULL), 0);
    CHECK_STR(json_key(o, 0), "a");
    CHECK(json_key(o, 1) == NULL);
    CHECK(json_key(a, 0) == NULL);
    CHECK(json_key(NULL, 0) == NULL);
    json_free(o); json_free(a); json_free(s); json_free(n);
}

static void test_type_predicates(void) {
    Json *o = json_object(), *a = json_array(), *n = json_null(), *s = json_string("");
    CHECK(json_is_null(NULL)); CHECK(json_is_null(n)); CHECK(!json_is_null(s)); CHECK(!json_is_null(o));
    CHECK(json_is_object(o)); CHECK(!json_is_object(a)); CHECK(!json_is_object(NULL));
    CHECK(json_is_array(a)); CHECK(!json_is_array(o)); CHECK(!json_is_array(NULL));
    json_free(o); json_free(a); json_free(n); json_free(s);
}

static void test_string_accessors_on_wrong_types(void) {
    Json *j = json_parsez("{\"s\":\"x\",\"e\":\"\",\"n\":1,\"b\":true,\"z\":null,\"a\":[\"x\"]}");
    CHECK_STR(json_str(json_get(j, "s")), "x");
    CHECK_STR(json_str(json_get(j, "e")), "");
    CHECK(json_str(json_get(j, "n")) == NULL);
    CHECK(json_str(json_get(j, "b")) == NULL);
    CHECK(json_str(json_get(j, "z")) == NULL);
    CHECK(json_str(json_get(j, "a")) == NULL);
    CHECK(json_str(json_get(j, "missing")) == NULL);
    CHECK(json_str(NULL) == NULL);
    CHECK_STR(json_str_nonempty(json_get(j, "s")), "x");
    CHECK(json_str_nonempty(json_get(j, "e")) == NULL);
    CHECK(json_str_nonempty(json_get(j, "n")) == NULL);
    CHECK_STR(json_str_or(json_get(j, "s"), "fb"), "x");
    CHECK_STR(json_str_or(json_get(j, "e"), "fb"), "");
    CHECK_STR(json_str_or(json_get(j, "n"), "fb"), "fb");
    CHECK_STR(json_str_or(json_get(j, "missing"), "fb"), "fb");
    CHECK(json_str_or(json_get(j, "missing"), NULL) == NULL);
    CHECK_OWNED_STR(json_dup_str(json_get(j, "s")), "x");
    CHECK_OWNED_STR(json_dup_str(json_get(j, "e")), "");
    CHECK(json_dup_str(json_get(j, "n")) == NULL);
    CHECK(json_dup_str(json_get(j, "missing")) == NULL);
    CHECK(json_dup_str(NULL) == NULL);
    char *dup = json_dup_str(json_get(j, "s"));
    CHECK(dup != json_str(json_get(j, "s")));
    free(dup);
    json_free(j);
}

static void test_dup_strings_skips_non_strings(void) {
    Json *a = json_parsez("[\"a\",1,null,\"\",{\"x\":\"y\"},[\"z\"],true,\"b\"]");
    size_t n = 99;
    char **items = json_dup_strings(a, &n);
    CHECK_INT(n, 3);
    CHECK_STR(items[0], "a"); CHECK_STR(items[1], ""); CHECK_STR(items[2], "b");
    str_array_free(items, n);
    json_free(a);

    a = json_array();
    items = json_dup_strings(a, &n);
    CHECK_INT(n, 0); CHECK(items != NULL);
    str_array_free(items, n);
    json_free(a);

    Json *o = json_parsez("{\"a\":\"x\"}");
    items = json_dup_strings(o, &n);
    CHECK_INT(n, 0);
    str_array_free(items, n);
    items = json_dup_strings(json_get(o, "missing"), &n);
    CHECK_INT(n, 0);
    str_array_free(items, n);
    items = json_dup_strings(NULL, &n);
    CHECK_INT(n, 0);
    str_array_free(items, n);
    json_free(o);
}

static void test_number_accessors(void) {
    Json *j = json_parsez("{\"n\":2.5,\"s\":\"3\",\"b\":true,\"z\":null}");
    double d = -1;
    CHECK(json_num(json_get(j, "n"), &d)); CHECK(d == 2.5);
    CHECK(json_num(json_get(j, "n"), NULL));
    d = -1;
    CHECK(!json_num(json_get(j, "s"), &d)); CHECK(d == -1);
    CHECK(!json_num(json_get(j, "b"), &d));
    CHECK(!json_num(json_get(j, "z"), &d));
    CHECK(!json_num(json_get(j, "missing"), &d));
    CHECK(!json_num(NULL, &d));
    CHECK(json_num_or(json_get(j, "n"), 9) == 2.5);
    CHECK(json_num_or(json_get(j, "s"), 9) == 9);
    CHECK(json_num_or(NULL, -3) == -3);
    json_free(j);
}

static void test_int_or_accepts_only_whole_numbers_in_range(void) {
    Json *v;
    v = json_number(42); CHECK_INT(json_int_or(v, -1), 42); json_free(v);
    v = json_number(-7); CHECK_INT(json_int_or(v, 0), -7); json_free(v);
    v = json_number(0); CHECK_INT(json_int_or(v, 5), 0); json_free(v);
    v = json_number(-0.0); CHECK_INT(json_int_or(v, 5), 0); json_free(v);
    v = json_number(2147483647.0); CHECK_INT(json_int_or(v, 0), 2147483647); json_free(v);
    v = json_number(-2147483648.0); CHECK_INT(json_int_or(v, 0), -2147483647 - 1); json_free(v);
    v = json_number(2147483648.0); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(-2147483649.0); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(1e300); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(1.5); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(-0.0001); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(INFINITY); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(-INFINITY); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_number(NAN); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_string("12"); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    v = json_bool(true); CHECK_INT(json_int_or(v, -1), -1); json_free(v);
    CHECK_INT(json_int_or(NULL, 77), 77);
    v = json_parsez("1e2"); CHECK_INT(json_int_or(v, -1), 100); json_free(v);
    v = json_parsez("3.0"); CHECK_INT(json_int_or(v, -1), 3); json_free(v);
}

static void test_bool_accessors(void) {
    Json *t = json_bool(true), *f = json_bool(false), *n = json_null(), *one = json_number(1), *s = json_string("true");
    CHECK(json_bool_is(t, true)); CHECK(!json_bool_is(t, false));
    CHECK(json_bool_is(f, false)); CHECK(!json_bool_is(f, true));
    CHECK(!json_bool_is(n, false)); CHECK(!json_bool_is(n, true));
    CHECK(!json_bool_is(one, true)); CHECK(!json_bool_is(s, true));
    CHECK(!json_bool_is(NULL, false));
    CHECK_INT(json_bool_tristate(t), 1);
    CHECK_INT(json_bool_tristate(f), 0);
    CHECK_INT(json_bool_tristate(n), -1);
    CHECK_INT(json_bool_tristate(one), -1);
    CHECK_INT(json_bool_tristate(s), -1);
    CHECK_INT(json_bool_tristate(NULL), -1);
    json_free(t); json_free(f); json_free(n); json_free(one); json_free(s);
}

static void test_is_set_follows_javascript_truthiness(void) {
    Json *j = json_parsez("{\"t\":true,\"f\":false,\"z\":null,\"n0\":0,\"n1\":1,\"neg\":-0.5,\"s\":\"x\",\"e\":\"\",\"zero\":\"0\",\"a\":[],\"o\":{}}");
    CHECK(json_is_set(json_get(j, "t")));
    CHECK(!json_is_set(json_get(j, "f")));
    CHECK(!json_is_set(json_get(j, "z")));
    CHECK(!json_is_set(json_get(j, "n0")));
    CHECK(json_is_set(json_get(j, "n1")));
    CHECK(json_is_set(json_get(j, "neg")));
    CHECK(json_is_set(json_get(j, "s")));
    CHECK(!json_is_set(json_get(j, "e")));
    CHECK(json_is_set(json_get(j, "zero")));
    CHECK(json_is_set(json_get(j, "a")));
    CHECK(json_is_set(json_get(j, "o")));
    CHECK(!json_is_set(json_get(j, "missing")));
    CHECK(!json_is_set(NULL));
    json_free(j);
}

static void test_setters(void) {
    Json *o = json_object();
    json_set_str(o, "s", "v");
    json_set_str(o, "skipped", NULL);
    json_set_num(o, "n", 3);
    json_set_bool(o, "b", false);
    CHECK_INT(json_count(o), 3);
    CHECK(json_is_null(json_get(o, "skipped")));
    json_set_str(o, "s", "w");
    json_set_str(o, "s", NULL);
    CHECK_STR(json_str(json_get(o, "s")), "w");
    json_set_num(o, "s", 4);
    CHECK_INT(json_int_or(json_get(o, "s"), 0), 4);
    CHECK_INT(json_count(o), 3);
    char *out = json_serialize(o, false);
    CHECK_STR(out, "{\"s\":4,\"n\":3,\"b\":false}");
    free(out);
    json_set_num(NULL, "x", 1);
    json_set_bool(NULL, "x", true);
    json_set_str(NULL, "x", "y");
    Json *a = json_array();
    json_set_str(a, "x", "y");
    CHECK_INT(json_count(a), 0);
    json_free(a);
    json_free(o);
}

static void test_equal_compares_structure_not_order(void) {
    Json *a = json_parsez("{\"x\":1,\"y\":[true,null,\"s\"],\"z\":{\"k\":2}}");
    Json *b = json_parsez("{\"z\":{\"k\":2},\"y\":[true,null,\"s\"],\"x\":1.0}");
    CHECK(json_equal(a, b));
    CHECK(json_equal(b, a));
    CHECK(json_equal(a, a));
    Json *c = json_parsez("{\"x\":1,\"y\":[true,null,\"s\"],\"z\":{\"k\":3}}");
    CHECK(!json_equal(a, c));
    Json *d = json_parsez("{\"x\":1,\"y\":[true,null,\"s\"]}");
    CHECK(!json_equal(a, d)); CHECK(!json_equal(d, a));
    Json *e = json_parsez("{\"x\":1,\"y\":[true,null,\"s\"],\"w\":{\"k\":2}}");
    CHECK(!json_equal(a, e));
    json_free(a); json_free(b); json_free(c); json_free(d); json_free(e);
}

static void test_equal_on_scalars_and_null(void) {
    Json *one = json_number(1), *one_b = json_number(1), *two = json_number(2), *s1 = json_string("1"), *t = json_bool(true), *f = json_bool(false);
    Json *n = json_null(), *a1 = json_parsez("[1,2]"), *a2 = json_parsez("[2,1]"), *a3 = json_parsez("[1]"), *o = json_object(), *arr = json_array();
    Json *pz = json_number(0.0), *nz = json_number(-0.0);
    CHECK(json_equal(one, one_b));
    CHECK(!json_equal(one, two));
    CHECK(!json_equal(one, s1));
    CHECK(!json_equal(t, f));
    CHECK(!json_equal(t, one));
    CHECK(json_equal(n, NULL));
    CHECK(json_equal(NULL, NULL));
    CHECK(!json_equal(NULL, f));
    CHECK(!json_equal(a1, a2));
    CHECK(!json_equal(a1, a3));
    CHECK(!json_equal(o, arr));
    CHECK(json_equal(pz, nz));
    Json *s2 = json_string("caf\xc3\xa9"), *s3 = json_string("cafe");
    CHECK(!json_equal(s2, s3));
    json_free(one); json_free(one_b); json_free(two); json_free(s1); json_free(t); json_free(f); json_free(n);
    json_free(a1); json_free(a2); json_free(a3); json_free(o); json_free(arr); json_free(pz); json_free(nz); json_free(s2); json_free(s3);
}

static void test_serialize_scalars(void) {
    Json *v;
    CHECK_OWNED_STR(json_serialize(NULL, false), "null");
    v = json_null(); CHECK_OWNED_STR(json_serialize(v, false), "null"); json_free(v);
    v = json_bool(true); CHECK_OWNED_STR(json_serialize(v, false), "true"); json_free(v);
    v = json_bool(false); CHECK_OWNED_STR(json_serialize(v, false), "false"); json_free(v);
    v = json_string(""); CHECK_OWNED_STR(json_serialize(v, false), "\"\""); json_free(v);
    v = json_array(); CHECK_OWNED_STR(json_serialize(v, true), "[]"); json_free(v);
    v = json_object(); CHECK_OWNED_STR(json_serialize(v, true), "{}"); json_free(v);
}

static void test_serialize_numbers_as_integers_or_shortest_fractions(void) {
    struct { double d; const char *text; } cases[] = {
        { 0, "0" }, { -0.0, "0" }, { 1, "1" }, { -1, "-1" }, { 42, "42" }, { 1700000000123.0, "1700000000123" },
        { 999999999999999.0, "999999999999999" }, { -999999999999999.0, "-999999999999999" }, { 1e15, "1e+15" },
        { 0.5, "0.5" }, { -2.25, "-2.25" }, { 0.1, "0.1" }, { 0.3, "0.3" }, { 1.0 / 3.0, "0.3333333333333333" },
        { 3.14159, "3.14159" }, { 1e-7, "1e-07" }, { 1.5e300, "1.5e+300" }, { 123.456, "123.456" },
        { 2147483648.0, "2147483648" }, { 0.1 + 0.2, "0.30000000000000004" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        Json *v = json_number(cases[i].d);
        CHECK_OWNED_STR(json_serialize(v, false), cases[i].text);
        json_free(v);
    }
}

static void test_serialized_numbers_round_trip(void) {
    double values[] = { 0.1, 1.0 / 3.0, 2.0 / 3.0, 1e-300, 4.9e-324, 1.7976931348623157e308, 123456.789, -98765.4321e-5, 1e15, 9007199254740993.0 };
    for (size_t i = 0; i < sizeof values / sizeof *values; i++) {
        Json *v = json_number(values[i]);
        char *text = json_serialize(v, false);
        Json *back = json_parsez(text);
        CHECK(back != NULL);
        CHECK(json_num_or(back, NAN) == values[i]);
        free(text); json_free(v); json_free(back);
    }
}

static void test_serialize_escapes_strings(void) {
    Json *v = json_string("q\" b\\ / \b\f\n\r\t");
    CHECK_OWNED_STR(json_serialize(v, false), "\"q\\\" b\\\\ / \\b\\f\\n\\r\\t\"");
    json_free(v);
    v = json_string("\x01\x1f\x7f");
    CHECK_OWNED_STR(json_serialize(v, false), "\"\\u0001\\u001f\x7f\"");
    json_free(v);
    // Non-ASCII is written as raw UTF-8, not \u escapes.
    v = json_string("caf\xc3\xa9 \xf0\x9f\x98\x80");
    CHECK_OWNED_STR(json_serialize(v, false), "\"caf\xc3\xa9 \xf0\x9f\x98\x80\"");
    json_free(v);
    v = json_object();
    json_set_num(v, "k\"\n", 1);
    CHECK_OWNED_STR(json_serialize(v, false), "{\"k\\\"\\n\":1}");
    json_free(v);
}

static void test_strings_round_trip_through_serialization(void) {
    const char *samples[] = { "", "plain", "\"\\/", "\b\f\n\r\t", "\x01\x02\x1e\x1f", "caf\xc3\xa9", "\xf0\x9f\x98\x80\xe2\x82\xac", "</script>" };
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        Json *v = json_string(samples[i]);
        char *text = json_serialize(v, false);
        Json *back = json_parsez(text);
        CHECK_STR(json_str(back), samples[i]);
        free(text); json_free(v); json_free(back);
    }
}

static void test_serialize_keeps_insertion_order_unless_sorted(void) {
    Json *o = json_object();
    json_set_num(o, "b", 1);
    json_set_num(o, "a", 2);
    json_set_num(o, "C", 3);
    Json *inner = json_object();
    json_set_num(inner, "z", 1); json_set_num(inner, "y", 2);
    json_object_set(o, "_", inner);
    CHECK_OWNED_STR(json_serialize(o, false), "{\"b\":1,\"a\":2,\"C\":3,\"_\":{\"z\":1,\"y\":2}}");
    CHECK_OWNED_STR(json_serialize(o, true), "{\"C\":3,\"_\":{\"y\":2,\"z\":1},\"a\":2,\"b\":1}");
    json_free(o);
}

static void test_sorted_serialization_makes_equal_values_equal_bytes(void) {
    Json *a = json_parsez("{\"b\":[{\"y\":1,\"x\":2}],\"a\":null}");
    Json *b = json_parsez("{\"a\":null,\"b\":[{\"x\":2,\"y\":1}]}");
    char *sa = json_serialize(a, true), *sb = json_serialize(b, true);
    CHECK_STR(sa, sb);
    CHECK_STR(sa, "{\"a\":null,\"b\":[{\"x\":2,\"y\":1}]}");
    free(sa); free(sb);
    // Sorting is by bytes, so UTF-8 keys come after ASCII.
    Json *c = json_parsez("{\"\xc3\xa9\":1,\"z\":2,\"aa\":3,\"a\":4}");
    CHECK_OWNED_STR(json_serialize(c, true), "{\"a\":4,\"aa\":3,\"z\":2,\"\xc3\xa9\":1}");
    json_free(a); json_free(b); json_free(c);
}

static void test_parse_then_serialize_is_stable(void) {
    CHECK_OWNED_STR(roundtrip(" [ 1 , 2.5 , -3e2 , \"\\u0041\" , true , false , null , { } , [ ] ] "), "[1,2.5,-300,\"A\",true,false,null,{},[]]");
    CHECK_OWNED_STR(roundtrip("{\"a\":{\"b\":{\"c\":[[[]]]}}}"), "{\"a\":{\"b\":{\"c\":[[[]]]}}}");
    CHECK_OWNED_STR(roundtrip("\"\\ud83d\\ude00\""), "\"\xf0\x9f\x98\x80\"");
    CHECK_OWNED_STR(roundtrip("1.0"), "1");
    CHECK_OWNED_STR(roundtrip("-0"), "0");
    CHECK_OWNED_STR(roundtrip("1E2"), "100");
    CHECK_OWNED_STR(roundtrip("\"\\/\""), "\"/\"");
}

static void test_pretty_output_is_indented_and_sorted(void) {
    Json *j = json_parsez("{\"b\":[1,{\"y\":true,\"x\":null}],\"a\":\"s\",\"e\":[],\"o\":{}}");
    CHECK_OWNED_STR(json_pretty(j),
        "{\n"
        "  \"a\": \"s\",\n"
        "  \"b\": [\n"
        "    1,\n"
        "    {\n"
        "      \"x\": null,\n"
        "      \"y\": true\n"
        "    }\n"
        "  ],\n"
        "  \"e\": [],\n"
        "  \"o\": {}\n"
        "}");
    json_free(j);
    Json *s = json_string("x\ny");
    CHECK_OWNED_STR(json_pretty(s), "\"x\\ny\"");
    json_free(s);
    CHECK_OWNED_STR(json_pretty(NULL), "null");
    Json *e = json_array();
    CHECK_OWNED_STR(json_pretty(e), "[]");
    json_free(e);
}

static void test_pretty_output_parses_back_to_an_equal_value(void) {
    Json *j = json_parsez("{\"k\":[1,2,{\"n\":[\"a\",\"b\"]}],\"t\":\"caf\xc3\xa9\\n\"}");
    char *pretty = json_pretty(j);
    Json *back = json_parsez(pretty);
    CHECK(json_equal(j, back));
    free(pretty); json_free(back); json_free(j);
}

static void test_serialize_long_strings(void) {
    char *big = xmalloc(70001);
    for (int i = 0; i < 70000; i++) big[i] = i % 100 == 99 ? '\n' : 'a';
    big[70000] = 0;
    Json *v = json_string(big);
    char *text = json_serialize(v, false);
    CHECK_INT(strlen(text), 70000 + 700 + 2);
    Json *back = json_parsez(text);
    CHECK_STR(json_str(back), big);
    free(text); json_free(v); json_free(back); free(big);
}

void json_tests(void) {
    test_run("parses scalars", test_parses_scalars);
    test_run("parses nested containers", test_parses_nested_containers);
    test_run("whitespace around the document is allowed", test_whitespace_around_the_document_is_allowed);
    test_run("trailing garbage is rejected", test_trailing_garbage_is_rejected);
    test_run("malformed documents are rejected", test_malformed_documents_are_rejected);
    test_run("NULL text is rejected", test_null_text_is_rejected);
    test_run("parse honours the length", test_parse_honours_the_length);
    test_run("an embedded NUL inside the length is rejected", test_an_embedded_nul_inside_the_length_is_rejected);
    test_run("a UTF-8 byte order mark is skipped", test_a_utf8_byte_order_mark_is_skipped);
    test_run("numbers in every form", test_numbers_in_every_form);
    test_run("malformed numbers are rejected", test_malformed_numbers_are_rejected);
    test_run("string escapes", test_string_escapes);
    test_run("raw UTF-8 passes through strings", test_raw_utf8_passes_through_strings);
    test_run("surrogate pairs decode to one code point", test_surrogate_pairs_decode_to_one_code_point);
    test_run("lone surrogates become the replacement character", test_lone_surrogates_become_the_replacement_character);
    test_run("bad escapes and control characters are rejected", test_bad_escapes_and_control_characters_are_rejected);
    test_run("duplicate keys keep the last value", test_duplicate_keys_keep_the_last_value);
    test_run("deep nesting is bounded", test_deep_nesting_is_bounded);
    test_run("nesting limit boundary", test_nesting_limit_boundary);
    test_run("large arrays and objects parse", test_large_arrays_and_objects_parse);
    test_run("constructors make each type", test_constructors_make_each_type);
    test_run("json_string copies its input", test_json_string_copies_its_input);
    test_run("array push takes ownership and grows", test_array_push_takes_ownership_and_grows);
    test_run("object set keeps insertion order and replaces", test_object_set_keeps_insertion_order_and_replaces);
    test_run("object set copies the key", test_object_set_copies_the_key);
    test_run("object remove", test_object_remove);
    test_run("object merge replaces and adds", test_object_merge_replaces_and_adds);
    test_run("clone is deep and equal", test_clone_is_deep_and_equal);
    test_run("subscripts never fail", test_subscripts_never_fail);
    test_run("count and key on every type", test_count_and_key_on_every_type);
    test_run("type predicates", test_type_predicates);
    test_run("string accessors on wrong types", test_string_accessors_on_wrong_types);
    test_run("dup strings skips non-strings", test_dup_strings_skips_non_strings);
    test_run("number accessors", test_number_accessors);
    test_run("int_or accepts only whole numbers in range", test_int_or_accepts_only_whole_numbers_in_range);
    test_run("bool accessors", test_bool_accessors);
    test_run("is_set follows JavaScript truthiness", test_is_set_follows_javascript_truthiness);
    test_run("setters", test_setters);
    test_run("equal compares structure not order", test_equal_compares_structure_not_order);
    test_run("equal on scalars and null", test_equal_on_scalars_and_null);
    test_run("serialize scalars", test_serialize_scalars);
    test_run("serialize numbers as integers or shortest fractions", test_serialize_numbers_as_integers_or_shortest_fractions);
    test_run("serialized numbers round trip", test_serialized_numbers_round_trip);
    test_run("serialize escapes strings", test_serialize_escapes_strings);
    test_run("strings round trip through serialization", test_strings_round_trip_through_serialization);
    test_run("serialize keeps insertion order unless sorted", test_serialize_keeps_insertion_order_unless_sorted);
    test_run("sorted serialization makes equal values equal bytes", test_sorted_serialization_makes_equal_values_equal_bytes);
    test_run("parse then serialize is stable", test_parse_then_serialize_is_stable);
    test_run("pretty output is indented and sorted", test_pretty_output_is_indented_and_sorted);
    test_run("pretty output parses back to an equal value", test_pretty_output_parses_back_to_an_equal_value);
    test_run("serialize long strings", test_serialize_long_strings);
}
