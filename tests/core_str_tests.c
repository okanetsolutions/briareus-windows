// str.c: the Str builder, allocation helpers, string predicates and transforms, and the UTF-8/UTF-16 edge.
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void test_a_new_builder_is_empty_and_detaches_to_an_empty_string(void) {
    Str s; str_init(&s);
    CHECK(s.data == NULL); CHECK_INT(s.len, 0); CHECK_INT(s.cap, 0);
    char *z = str_detach(&s);
    CHECK(z != NULL); CHECK_STR(z, "");
    CHECK(s.data == NULL); CHECK_INT(s.len, 0); CHECK_INT(s.cap, 0);
    free(z);
}

static void test_appends_keep_the_buffer_terminated(void) {
    Str s; str_init(&s);
    str_append(&s, "abcdef", 3);
    CHECK_INT(s.len, 3); CHECK_STR(s.data, "abc");
    str_appendz(&s, "de");
    str_appendz(&s, NULL);
    str_appendz(&s, "");
    str_appendc(&s, 'f');
    str_append(&s, "zzz", 0);
    CHECK_INT(s.len, 6); CHECK_STR(s.data, "abcdef");
    CHECK(s.cap > s.len);
    CHECK_OWNED_STR(str_detach(&s), "abcdef");
    CHECK(s.data == NULL);
}

static void test_the_builder_grows_past_its_first_capacity(void) {
    Str s; str_init(&s);
    for (int i = 0; i < 10000; i++) str_appendc(&s, (char)('a' + i % 26));
    CHECK_INT(s.len, 10000);
    CHECK(s.cap >= 10001);
    CHECK_INT(strlen(s.data), 10000);
    bool ok = true;
    for (int i = 0; i < 10000; i++) if (s.data[i] != (char)('a' + i % 26)) ok = false;
    CHECK(ok);
    str_free(&s);
    CHECK(s.data == NULL); CHECK_INT(s.len, 0); CHECK_INT(s.cap, 0);
}

static void test_reserve_makes_room_without_changing_the_content(void) {
    Str s; str_init(&s);
    str_reserve(&s, 0);
    CHECK(s.cap >= 1); CHECK_INT(s.len, 0);
    str_appendz(&s, "hi");
    str_reserve(&s, 1000);
    CHECK(s.cap >= 1003); CHECK_INT(s.len, 2);
    size_t cap = s.cap;
    str_reserve(&s, 10);
    CHECK_INT(s.cap, cap);
    CHECK_STR(s.data, "hi");
    str_free(&s);
}

static void test_an_embedded_nul_is_kept_by_length(void) {
    Str s; str_init(&s);
    str_append(&s, "a\0b", 3);
    CHECK_INT(s.len, 3);
    CHECK(memcmp(s.data, "a\0b", 4) == 0);
    str_free(&s);
}

static void test_appendf_formats_and_appends(void) {
    Str s; str_init(&s);
    str_appendz(&s, "n=");
    str_appendf(&s, "%d %s %.2f %c", 42, "x", 1.5, '!');
    CHECK_STR(s.data, "n=42 x 1.50 !");
    CHECK_INT(s.len, strlen("n=42 x 1.50 !"));
    str_appendf(&s, "%s", "");
    CHECK_INT(s.len, strlen("n=42 x 1.50 !"));
    str_free(&s);
}

static void test_appendf_handles_output_larger_than_the_buffer(void) {
    char *big = xmalloc(100001);
    memset(big, 'q', 100000); big[100000] = 0;
    Str s; str_init(&s);
    str_appendz(&s, "<");
    str_appendf(&s, "%s>%d", big, 7);
    CHECK_INT(s.len, 100003);
    CHECK(s.data[0] == '<'); CHECK(s.data[100000] == 'q'); CHECK(s.data[100001] == '>'); CHECK(s.data[100002] == '7');
    CHECK(s.data[100003] == 0);
    str_free(&s);
    free(big);
}

static void test_xstrfmt_returns_a_new_formatted_string(void) {
    CHECK_OWNED_STR(xstrfmt("%s-%03d", "id", 5), "id-005");
    CHECK_OWNED_STR(xstrfmt("%s", ""), "");
    char *long_one = xstrfmt("%0500d", 1);
    CHECK_INT(strlen(long_one), 500);
    CHECK(long_one[499] == '1' && long_one[0] == '0');
    free(long_one);
}

static void test_allocation_helpers_accept_zero_and_null(void) {
    void *p = xmalloc(0); CHECK(p != NULL); free(p);
    p = xcalloc(0, 8); CHECK(p != NULL); free(p);
    p = xcalloc(4, 0); CHECK(p != NULL); free(p);
    int *z = xcalloc(4, sizeof *z);
    CHECK(z[0] == 0 && z[3] == 0);
    z = xrealloc(z, 8 * sizeof *z);
    CHECK(z[0] == 0 && z[3] == 0);
    z = xrealloc(z, 0);
    CHECK(z != NULL);
    free(z);
    CHECK(xstrdup(NULL) == NULL);
    CHECK_OWNED_STR(xstrdup(""), "");
    CHECK_OWNED_STR(xstrdup("caf\xc3\xa9"), "caf\xc3\xa9");
}

static void test_a_kept_block_is_freed_with_xfree_kept(void) {
    // Under the debug CRT a kept block is a client block, and plain free() would assert on its type.
    char *p = xmalloc_kept(0); CHECK(p != NULL); xfree_kept(p);
    p = xmalloc_kept(4); memcpy(p, "abc", 4); CHECK_STR(p, "abc"); xfree_kept(p);
    xfree_kept(NULL);
}

static void test_xstrndup_copies_a_prefix(void) {
    CHECK_OWNED_STR(xstrndup("abcdef", 3), "abc");
    CHECK_OWNED_STR(xstrndup("abcdef", 0), "");
    CHECK_OWNED_STR(xstrndup("ab\0cd", 2), "ab");
}

static void test_empty_is_true_for_null_and_the_empty_string(void) {
    CHECK(str_empty(NULL));
    CHECK(str_empty(""));
    CHECK(!str_empty(" "));
    CHECK(!str_empty("a"));
}

static void test_trim_strips_ascii_whitespace_at_both_ends(void) {
    CHECK_OWNED_STR(str_trim("  hello world \t\r\n"), "hello world");
    CHECK_OWNED_STR(str_trim("\f\vx\v\f"), "x");
    CHECK_OWNED_STR(str_trim("inner  space"), "inner  space");
    CHECK_OWNED_STR(str_trim("   "), "");
    CHECK_OWNED_STR(str_trim(""), "");
    CHECK_OWNED_STR(str_trim(NULL), "");
    // Non-breaking space is not ASCII whitespace.
    CHECK_OWNED_STR(str_trim("\xc2\xa0x\xc2\xa0"), "\xc2\xa0x\xc2\xa0");
    CHECK_OWNED_STR(str_trim(" \xc3\xa9 "), "\xc3\xa9");
}

static void test_fold_lowercases_ascii_only(void) {
    CHECK_OWNED_STR(str_fold("Hello WORLD 123"), "hello world 123");
    CHECK_OWNED_STR(str_fold("\xc3\x89T\xc3\x89"), "\xc3\x89t\xc3\x89");
    CHECK_OWNED_STR(str_fold(""), "");
    CHECK_OWNED_STR(str_fold(NULL), "");
}

static void test_eq_and_ieq_handle_null(void) {
    CHECK(str_eq("a", "a"));
    CHECK(!str_eq("a", "A"));
    CHECK(!str_eq("a", "ab"));
    CHECK(str_eq("", ""));
    CHECK(str_eq(NULL, NULL));
    CHECK(!str_eq(NULL, ""));
    CHECK(!str_eq("", NULL));
    CHECK(str_ieq("Hello", "hELLO"));
    CHECK(!str_ieq("Hello", "Hell"));
    CHECK(!str_ieq("Hell", "Hello"));
    CHECK(str_ieq("", ""));
    CHECK(str_ieq(NULL, NULL));
    CHECK(!str_ieq(NULL, "a"));
    CHECK(!str_ieq("a", NULL));
    CHECK(str_ieq("\xc3\xa9", "\xc3\xa9"));
    CHECK(!str_ieq("\xc3\xa9", "\xc3\x89"));
}

static void test_prefix_and_suffix(void) {
    CHECK(str_has_prefix("https://x", "https://"));
    CHECK(str_has_prefix("abc", ""));
    CHECK(str_has_prefix("abc", "abc"));
    CHECK(!str_has_prefix("ab", "abc"));
    CHECK(!str_has_prefix("abc", "B"));
    CHECK(!str_has_prefix(NULL, "a"));
    CHECK(!str_has_prefix("a", NULL));
    CHECK(str_has_suffix("file.png", ".png"));
    CHECK(str_has_suffix("abc", ""));
    CHECK(str_has_suffix("abc", "abc"));
    CHECK(!str_has_suffix("bc", "abc"));
    CHECK(!str_has_suffix("file.PNG", ".png"));
    CHECK(!str_has_suffix(NULL, "a"));
    CHECK(!str_has_suffix("a", NULL));
    CHECK(str_has_suffix("", ""));
}

static void test_icontains_ignores_ascii_case(void) {
    CHECK(str_icontains("Hello World", "lo wo"));
    CHECK(str_icontains("Hello World", "WORLD"));
    CHECK(str_icontains("Hello", "hello"));
    CHECK(str_icontains("Hello", ""));
    CHECK(str_icontains("", ""));
    CHECK(!str_icontains("", "a"));
    CHECK(!str_icontains("Hello", "Hello!"));
    CHECK(!str_icontains("abc", "abd"));
    CHECK(str_icontains("aaab", "aab"));
    CHECK(!str_icontains(NULL, "a"));
    CHECK(!str_icontains("a", NULL));
    CHECK(str_icontains("caf\xc3\xa9 au lait", "\xc3\xa9 AU"));
}

static void test_replace_every_occurrence(void) {
    CHECK_OWNED_STR(str_replace("a-b-c", "-", "+"), "a+b+c");
    CHECK_OWNED_STR(str_replace("a-b-c", "-", ""), "abc");
    CHECK_OWNED_STR(str_replace("a-b-c", "-", "--"), "a--b--c");
    CHECK_OWNED_STR(str_replace("aaaa", "aa", "b"), "bb");
    CHECK_OWNED_STR(str_replace("aaa", "aa", "b"), "ba");
    CHECK_OWNED_STR(str_replace("abc", "x", "y"), "abc");
    CHECK_OWNED_STR(str_replace("abc", "abc", ""), "");
    CHECK_OWNED_STR(str_replace("", "a", "b"), "");
    CHECK_OWNED_STR(str_replace("abc", "", "x"), "abc");
    CHECK_OWNED_STR(str_replace("ab", "abc", "x"), "ab");
    CHECK_OWNED_STR(str_replace("\xc3\xa9t\xc3\xa9", "\xc3\xa9", "e"), "ete");
    CHECK_OWNED_STR(str_replace("a\nb\n", "\n", "\r\n"), "a\r\nb\r\n");
}

static void test_capitalized_uppercases_the_first_ascii_letter(void) {
    CHECK_OWNED_STR(str_capitalized("running"), "Running");
    CHECK_OWNED_STR(str_capitalized("Done"), "Done");
    CHECK_OWNED_STR(str_capitalized("a"), "A");
    CHECK_OWNED_STR(str_capitalized("1abc"), "1abc");
    CHECK_OWNED_STR(str_capitalized("two words"), "Two words");
    CHECK_OWNED_STR(str_capitalized("\xc3\xa9t\xc3\xa9"), "\xc3\xa9t\xc3\xa9");
    CHECK_OWNED_STR(str_capitalized(""), "");
    CHECK_OWNED_STR(str_capitalized(NULL), "");
}

static void test_split_keeps_empty_fields(void) {
    size_t n = 99;
    char **parts = str_split("a,b,,c", ',', &n);
    CHECK_INT(n, 4);
    CHECK_STR(parts[0], "a"); CHECK_STR(parts[1], "b"); CHECK_STR(parts[2], ""); CHECK_STR(parts[3], "c");
    CHECK(parts[4] == NULL);
    str_array_free(parts, n);

    parts = str_split(",x,", ',', &n);
    CHECK_INT(n, 3);
    CHECK_STR(parts[0], ""); CHECK_STR(parts[1], "x"); CHECK_STR(parts[2], "");
    CHECK(parts[3] == NULL);
    str_array_free(parts, n);
}

static void test_split_of_nothing_is_one_empty_field(void) {
    size_t n = 99;
    char **parts = str_split("", '\n', &n);
    CHECK_INT(n, 1); CHECK_STR(parts[0], ""); CHECK(parts[1] == NULL);
    str_array_free(parts, n);
    parts = str_split(NULL, '\n', &n);
    CHECK_INT(n, 1); CHECK_STR(parts[0], ""); CHECK(parts[1] == NULL);
    str_array_free(parts, n);
    parts = str_split("whole", ',', &n);
    CHECK_INT(n, 1); CHECK_STR(parts[0], "whole"); CHECK(parts[1] == NULL);
    str_array_free(parts, n);
}

static void test_split_grows_for_many_fields(void) {
    Str s; str_init(&s);
    for (int i = 0; i < 1000; i++) str_appendf(&s, i ? "\n%d" : "%d", i);
    size_t n = 0;
    char **parts = str_split(s.data, '\n', &n);
    CHECK_INT(n, 1000);
    CHECK_STR(parts[0], "0"); CHECK_STR(parts[7], "7"); CHECK_STR(parts[8], "8"); CHECK_STR(parts[999], "999");
    CHECK(parts[1000] == NULL);
    // The boundary where the array first grows: exactly 7 and 8 fields.
    str_array_free(parts, n);
    parts = str_split("1,2,3,4,5,6,7", ',', &n);
    CHECK_INT(n, 7); CHECK_STR(parts[6], "7"); CHECK(parts[7] == NULL);
    str_array_free(parts, n);
    parts = str_split("1,2,3,4,5,6,7,8", ',', &n);
    CHECK_INT(n, 8); CHECK_STR(parts[7], "8"); CHECK(parts[8] == NULL);
    str_array_free(parts, n);
    str_free(&s);
}

static void test_split_on_multibyte_text(void) {
    size_t n = 0;
    char **parts = str_split("\xc3\xa9|\xf0\x9f\x98\x80|z", '|', &n);
    CHECK_INT(n, 3);
    CHECK_STR(parts[0], "\xc3\xa9"); CHECK_STR(parts[1], "\xf0\x9f\x98\x80"); CHECK_STR(parts[2], "z");
    str_array_free(parts, n);
}

static void test_array_free_accepts_null(void) {
    str_array_free(NULL, 0);
    str_array_free(NULL, 5);
    char **empty = xmalloc(sizeof *empty);
    str_array_free(empty, 0);
    CHECK(true);
}

static void test_utf8_to_wide_converts_multibyte_and_emoji(void) {
    wchar_t *w = utf8_to_wide("caf\xc3\xa9 \xf0\x9f\x98\x80");
    CHECK(wcscmp(w, L"caf\x00e9 \xd83d\xde00") == 0);
    CHECK_INT(wcslen(w), 7);
    free(w);
    w = utf8_to_wide("");
    CHECK(w != NULL && w[0] == 0);
    free(w);
    w = utf8_to_wide(NULL);
    CHECK(w != NULL && w[0] == 0);
    free(w);
    w = utf8_to_wide("\xe2\x82\xac");
    CHECK(wcscmp(w, L"\x20ac") == 0);
    free(w);
}

static void test_wide_to_utf8_converts_surrogate_pairs(void) {
    CHECK_OWNED_STR(wide_to_utf8(L"caf\x00e9 \xd83d\xde00"), "caf\xc3\xa9 \xf0\x9f\x98\x80");
    CHECK_OWNED_STR(wide_to_utf8(L"plain"), "plain");
    CHECK_OWNED_STR(wide_to_utf8(L""), "");
    CHECK_OWNED_STR(wide_to_utf8(NULL), "");
}

static void test_utf8_and_wide_round_trip(void) {
    const char *samples[] = { "", "ascii", "\xc3\xa9\xc3\xa8\xc3\xaa", "\xe4\xb8\xad\xe6\x96\x87",
                              "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", "mix \xe2\x9c\x93 \xf0\x9f\x9a\x80 end", "line\r\nbreak\ttab" };
    for (size_t i = 0; i < sizeof samples / sizeof *samples; i++) {
        wchar_t *w = utf8_to_wide(samples[i]);
        CHECK_OWNED_STR(wide_to_utf8(w), samples[i]);
        free(w);
    }
    char *big = xmalloc(30001);
    for (int i = 0; i < 10000; i++) memcpy(big + i * 3, "\xe2\x82\xac", 3);
    big[30000] = 0;
    wchar_t *w = utf8_to_wide(big);
    CHECK_INT(wcslen(w), 10000);
    CHECK_OWNED_STR(wide_to_utf8(w), big);
    free(w);
    free(big);
}

void str_tests(void) {
    test_run("a new builder is empty and detaches to an empty string", test_a_new_builder_is_empty_and_detaches_to_an_empty_string);
    test_run("appends keep the buffer terminated", test_appends_keep_the_buffer_terminated);
    test_run("the builder grows past its first capacity", test_the_builder_grows_past_its_first_capacity);
    test_run("reserve makes room without changing the content", test_reserve_makes_room_without_changing_the_content);
    test_run("an embedded NUL is kept by length", test_an_embedded_nul_is_kept_by_length);
    test_run("appendf formats and appends", test_appendf_formats_and_appends);
    test_run("appendf handles output larger than the buffer", test_appendf_handles_output_larger_than_the_buffer);
    test_run("xstrfmt returns a new formatted string", test_xstrfmt_returns_a_new_formatted_string);
    test_run("allocation helpers accept zero and NULL", test_allocation_helpers_accept_zero_and_null);
    test_run("a kept block is freed with xfree_kept", test_a_kept_block_is_freed_with_xfree_kept);
    test_run("xstrndup copies a prefix", test_xstrndup_copies_a_prefix);
    test_run("empty is true for NULL and the empty string", test_empty_is_true_for_null_and_the_empty_string);
    test_run("trim strips ASCII whitespace at both ends", test_trim_strips_ascii_whitespace_at_both_ends);
    test_run("fold lowercases ASCII only", test_fold_lowercases_ascii_only);
    test_run("eq and ieq handle NULL", test_eq_and_ieq_handle_null);
    test_run("prefix and suffix", test_prefix_and_suffix);
    test_run("icontains ignores ASCII case", test_icontains_ignores_ascii_case);
    test_run("replace every occurrence", test_replace_every_occurrence);
    test_run("capitalized uppercases the first ASCII letter", test_capitalized_uppercases_the_first_ascii_letter);
    test_run("split keeps empty fields", test_split_keeps_empty_fields);
    test_run("split of nothing is one empty field", test_split_of_nothing_is_one_empty_field);
    test_run("split grows for many fields", test_split_grows_for_many_fields);
    test_run("split on multibyte text", test_split_on_multibyte_text);
    test_run("array free accepts NULL", test_array_free_accepts_null);
    test_run("utf8 to wide converts multibyte and emoji", test_utf8_to_wide_converts_multibyte_and_emoji);
    test_run("wide to utf8 converts surrogate pairs", test_wide_to_utf8_converts_surrogate_pairs);
    test_run("utf8 and wide round trip", test_utf8_and_wide_round_trip);
}
