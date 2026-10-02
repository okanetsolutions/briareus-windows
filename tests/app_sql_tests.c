// The Database tab's SQL over SSH: mysql --batch output split into cells, and the quoting around names and logins.
#include "ssh_query.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void test_batch_output_splits_into_header_and_rows(void) {
    SqlTable t;
    sql_table_parse("id\tname\tnote\n1\tAda\tNULL\n2\tGrace\ta\\tb\\nc \\\\ d\n", &t);
    CHECK_INT(t.cols, 3);
    CHECK_INT(t.rows, 3);
    CHECK_STR(sql_cell(&t, 0, 1), "name");
    CHECK_STR(sql_cell(&t, 1, 2), "NULL");
    CHECK_STR(sql_cell(&t, 2, 1), "Grace");
    CHECK_STR(sql_cell(&t, 2, 2), "a\tb\nc \\ d");
    sql_table_free(&t);
}

static void test_one_column_keeps_empty_values_and_crlf(void) {
    SqlTable t;
    sql_table_parse("Database\r\ninformation_schema\r\n\r\nshop\r\n", &t);
    CHECK_INT(t.cols, 1);
    CHECK_INT(t.rows, 4);
    CHECK_STR(sql_cell(&t, 1, 0), "information_schema");
    CHECK_STR(sql_cell(&t, 2, 0), "");
    CHECK_STR(sql_cell(&t, 3, 0), "shop");
    sql_table_free(&t);
}

static void test_short_rows_are_padded_and_empty_output_has_no_rows(void) {
    SqlTable t;
    sql_table_parse("a\tb\tc\n1\n", &t);
    CHECK_INT(t.rows, 2);
    CHECK_STR(sql_cell(&t, 1, 0), "1");
    CHECK_STR(sql_cell(&t, 1, 2), "");
    sql_table_free(&t);
    sql_table_parse("", &t);
    CHECK_INT(t.rows, 0);
    sql_table_free(&t);
}

static void test_names_and_logins_are_quoted(void) {
    CHECK_OWNED_STR(sql_ident("my`table"), "`my``table`");
    CHECK_OWNED_STR(sh_quote("it's"), "'it'\"'\"'s'");
    SqlLogin login = { "", 0, "o'neil", "secret" };
    char *cmd = ssh_query_remote_command(&login);
    CHECK(strstr(cmd, "-h '127.0.0.1' -P 3306 -u 'o'\"'\"'neil'") != NULL);
    // No backslash, which Git's msys ssh.exe would read its own way.
    CHECK(strchr(cmd, '\\') == NULL);
    // The password goes in through standard input, never on the command line.
    CHECK(strstr(cmd, "secret") == NULL);
    free(cmd);
}

void app_sql_tests(void) {
    test_run("batch output splits into header and rows", test_batch_output_splits_into_header_and_rows);
    test_run("one column keeps empty values and crlf", test_one_column_keeps_empty_values_and_crlf);
    test_run("short rows are padded and empty output has no rows", test_short_rows_are_padded_and_empty_output_has_no_rows);
    test_run("names and logins are quoted", test_names_and_logins_are_quoted);
}
