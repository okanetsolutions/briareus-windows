#include "diff.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

// Lines written out one per row as "kind old new text", kinds as @ + - = \.
static char *diff_desc(const char *patch) {
    size_t n; DiffLine *lines = diff_parse(patch, &n);
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) {
        static const char kinds[] = { '@', '+', '-', '=', '\\' };
        str_appendf(&s, "%c %d %d %s\n", kinds[lines[i].kind], lines[i].old_line, lines[i].new_line, lines[i].text);
    }
    diff_free(lines, n);
    return str_detach(&s);
}

static void test_null_and_empty_patches_have_no_lines(void) {
    size_t n = 99; DiffLine *lines = diff_parse(NULL, &n);
    CHECK_INT(n, 0); CHECK(lines == NULL);
    diff_free(lines, n);
    // An empty patch still frees cleanly (xcalloc of zero lines, #46).
    n = 99; lines = diff_parse("", &n); CHECK_INT(n, 0); diff_free(lines, n);
    diff_free(NULL, 0);
}

static void test_a_lone_newline_is_one_empty_context_line(void) {
    CHECK_OWNED_STR(diff_desc("\n"), "= 0 0 \n");
}

static void test_counts_in_hunk_headers_are_optional(void) {
    CHECK_OWNED_STR(diff_desc("@@ -5 +7 @@\n a\n-b\n+c"), "@ 0 0 @@ -5 +7 @@\n= 5 7 a\n- 6 0 b\n+ 0 8 c\n");
    CHECK_OWNED_STR(diff_desc("@@ -5,2 +7 @@\n a"), "@ 0 0 @@ -5,2 +7 @@\n= 5 7 a\n");
    CHECK_OWNED_STR(diff_desc("@@ -5 +7,3 @@\n a"), "@ 0 0 @@ -5 +7,3 @@\n= 5 7 a\n");
    // Without the closing @@ the numbers still read.
    CHECK_OWNED_STR(diff_desc("@@ -3 +4\n a"), "@ 0 0 @@ -3 +4\n= 3 4 a\n");
}

static void test_hunk_text_keeps_the_whole_header_with_its_context(void) {
    size_t n; DiffLine *lines = diff_parse("@@ -1,2 +1,2 @@ static int main(void) {\n x", &n);
    CHECK_INT(n, 2);
    if (n == 2) { CHECK(lines[0].kind == DIFF_HUNK); CHECK_STR(lines[0].text, "@@ -1,2 +1,2 @@ static int main(void) {"); CHECK_INT(lines[0].old_line, 0); CHECK_INT(lines[0].new_line, 0); }
    diff_free(lines, n);
}

static void test_added_removed_and_context_lines_count_their_own_sides(void) {
    CHECK_OWNED_STR(diff_desc("@@ -10,4 +20,5 @@\n keep\n-gone1\n-gone2\n+new1\n+new2\n+new3\n keep2\n-last"),
                    "@ 0 0 @@ -10,4 +20,5 @@\n= 10 20 keep\n- 11 0 gone1\n- 12 0 gone2\n+ 0 21 new1\n+ 0 22 new2\n+ 0 23 new3\n= 13 24 keep2\n- 14 0 last\n");
}

static void test_each_hunk_restarts_the_numbering(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1,2 +1,2 @@\n-a\n+b\n c\n@@ -50,1 +60,2 @@\n x\n+y\n@@ -100 +101 @@\n-z"),
                    "@ 0 0 @@ -1,2 +1,2 @@\n- 1 0 a\n+ 0 1 b\n= 2 2 c\n"
                    "@ 0 0 @@ -50,1 +60,2 @@\n= 50 60 x\n+ 0 61 y\n"
                    "@ 0 0 @@ -100 +101 @@\n- 100 0 z\n");
}

static void test_new_and_deleted_files_number_from_their_single_side(void) {
    CHECK_OWNED_STR(diff_desc("@@ -0,0 +1,3 @@\n+one\n+two\n+three\n"), "@ 0 0 @@ -0,0 +1,3 @@\n+ 0 1 one\n+ 0 2 two\n+ 0 3 three\n");
    CHECK_OWNED_STR(diff_desc("@@ -1,2 +0,0 @@\n-one\n-two\n"), "@ 0 0 @@ -1,2 +0,0 @@\n- 1 0 one\n- 2 0 two\n");
}

static void test_no_newline_notes_take_no_line_number(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n-old\n\\ No newline at end of file\n+new\n\\ No newline at end of file\n"),
                    "@ 0 0 @@ -1 +1 @@\n- 1 0 old\n\\ 0 0 No newline at end of file\n+ 0 1 new\n\\ 0 0 No newline at end of file\n");
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n\\No space\n\\   many\n\\"), "@ 0 0 @@ -1 +1 @@\n\\ 0 0 No space\n\\ 0 0 many\n\\ 0 0 \n");
    // A note between context lines leaves the count running.
    CHECK_OWNED_STR(diff_desc("@@ -7 +7 @@\n a\n\\ note\n b"), "@ 0 0 @@ -7 +7 @@\n= 7 7 a\n\\ 0 0 note\n= 8 8 b\n");
}

static void test_line_text_drops_only_the_first_column(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n+  indented\n-\t tab\n   two spaces\n+\n-\n \n++x\n--y"),
                    "@ 0 0 @@ -1 +1 @@\n+ 0 1   indented\n- 1 0 \t tab\n= 2 2   two spaces\n+ 0 3 \n- 3 0 \n= 4 4 \n+ 0 5 +x\n- 5 0 -y\n");
}

static void test_empty_lines_inside_a_hunk_are_context(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1,3 +1,3 @@\n a\n\n b"), "@ 0 0 @@ -1,3 +1,3 @@\n= 1 1 a\n= 2 2 \n= 3 3 b\n");
}

static void test_only_one_final_newline_is_dropped(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n a\n"), "@ 0 0 @@ -1 +1 @@\n= 1 1 a\n");
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n a\n\n"), "@ 0 0 @@ -1 +1 @@\n= 1 1 a\n= 2 2 \n");
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n a"), "@ 0 0 @@ -1 +1 @@\n= 1 1 a\n");
}

static void test_crlf_and_lone_cr_end_lines_like_lf(void) {
    CHECK_OWNED_STR(diff_desc("@@ -1,2 +1,2 @@\r\n a\r\n-b\r\n+c\r\n"), "@ 0 0 @@ -1,2 +1,2 @@\n= 1 1 a\n- 2 0 b\n+ 0 2 c\n");
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\r-b\r+c\r"), "@ 0 0 @@ -1 +1 @@\n- 1 0 b\n+ 0 1 c\n");
    // CR LF is one break, LF CR is two.
    CHECK_OWNED_STR(diff_desc("@@ -1 +1 @@\n a\n\r b"), "@ 0 0 @@ -1 +1 @@\n= 1 1 a\n= 2 2 \n= 3 3 b\n");
    size_t n; DiffLine *lines = diff_parse("+x\r\n", &n);
    CHECK_INT(n, 1);
    if (n == 1) CHECK(strchr(lines[0].text, '\r') == NULL);
    diff_free(lines, n);
}

static void test_malformed_hunk_headers_keep_the_running_numbers(void) {
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ bogus @@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ bogus @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ -x +1 @@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ -x +1 @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ -1 +y @@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ -1 +y @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ -1;2 +1 @@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ -1;2 +1 @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ +1 -1 @@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ +1 -1 @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@\n= 4 5 b\n");
    CHECK_OWNED_STR(diff_desc("@@ -3 +4 @@\n a\n@@ -9\n b"), "@ 0 0 @@ -3 +4 @@\n= 3 4 a\n@ 0 0 @@ -9\n= 4 5 b\n");
}

static void test_ids_follow_the_line_order(void) {
    size_t n; DiffLine *lines = diff_parse("@@ -1 +1 @@\n a\n\\ note\n-b\n+c\n@@ -9 +9 @@\n d", &n);
    CHECK_INT(n, 7);
    for (size_t i = 0; i < n; i++) CHECK_INT(lines[i].id, i);
    diff_free(lines, n);
}

static void test_long_patches_keep_every_line(void) {
    Str s; str_init(&s);
    str_appendz(&s, "@@ -1,300 +1,300 @@\n");
    for (int i = 0; i < 300; i++) str_appendf(&s, "%c%d\n", i % 3 == 0 ? ' ' : i % 3 == 1 ? '-' : '+', i);
    char *patch = str_detach(&s);
    size_t n; DiffLine *lines = diff_parse(patch, &n);
    CHECK_INT(n, 301);
    if (n == 301) {
        CHECK_INT(lines[300].id, 300); CHECK(lines[300].kind == DIFF_ADDED); CHECK_STR(lines[300].text, "299"); CHECK_INT(lines[300].new_line, 200);
        CHECK(lines[298].kind == DIFF_CONTEXT); CHECK_INT(lines[298].old_line, 199); CHECK_INT(lines[298].new_line, 199);
        CHECK(lines[299].kind == DIFF_REMOVED); CHECK_INT(lines[299].old_line, 200);
    }
    diff_free(lines, n); free(patch);
}

void diff_tests(void) {
    test_run("null and empty patches have no lines", test_null_and_empty_patches_have_no_lines);
    test_run("a lone newline is one empty context line", test_a_lone_newline_is_one_empty_context_line);
    test_run("counts in hunk headers are optional", test_counts_in_hunk_headers_are_optional);
    test_run("hunk text keeps the whole header with its context", test_hunk_text_keeps_the_whole_header_with_its_context);
    test_run("added, removed and context lines count their own sides", test_added_removed_and_context_lines_count_their_own_sides);
    test_run("each hunk restarts the numbering", test_each_hunk_restarts_the_numbering);
    test_run("new and deleted files number from their single side", test_new_and_deleted_files_number_from_their_single_side);
    test_run("no newline notes take no line number", test_no_newline_notes_take_no_line_number);
    test_run("line text drops only the first column", test_line_text_drops_only_the_first_column);
    test_run("empty lines inside a hunk are context", test_empty_lines_inside_a_hunk_are_context);
    test_run("only one final newline is dropped", test_only_one_final_newline_is_dropped);
    test_run("CRLF and lone CR end lines like LF", test_crlf_and_lone_cr_end_lines_like_lf);
    test_run("malformed hunk headers keep the running numbers", test_malformed_hunk_headers_keep_the_running_numbers);
    test_run("ids follow the line order", test_ids_follow_the_line_order);
    test_run("long patches keep every line", test_long_patches_keep_every_line);
}
