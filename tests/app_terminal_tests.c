// terminal.c: the command lines sessions run, a database session's remote mysql command included.
#include "str.h"
#include "suites.h"
#include "terminal.h"
#include "test.h"
#include <stdlib.h>

static char *line_for(const TermTarget *target) {
    wchar_t *w = term_command_line(L"C:\\Git\\usr\\bin\\ssh.exe", target);
    char *line = wide_to_utf8(w);
    free(w);
    return line;
}

static void test_a_shell_session_runs_ssh_to_the_server(void) {
    TermTarget t = { "ssh:1", "o/r", "Web", "deploy", "web.example.com", 2222, NULL, NULL };
    CHECK_OWNED_STR(line_for(&t), "\"C:\\Git\\usr\\bin\\ssh.exe\" -p 2222 -l deploy -- web.example.com");
}

static void test_a_remote_command_gets_a_terminal_and_one_quoted_argument(void) {
    TermTarget t = { "db:1", "o/r", "Web", "deploy", "web.example.com", 22, "mysql --user='app' --password", "secret" };
    CHECK_OWNED_STR(line_for(&t), "\"C:\\Git\\usr\\bin\\ssh.exe\" -t -p 22 -l deploy -- web.example.com \"mysql --user='app' --password\"");
    // Quotes and the backslashes before them are escaped as the C runtime splits a command line; others stay.
    t.command = "a\\b \"c\" d\\\\\"e\\";
    CHECK_OWNED_STR(line_for(&t), "\"C:\\Git\\usr\\bin\\ssh.exe\" -t -p 22 -l deploy -- web.example.com \"a\\b \\\"c\\\" d\\\\\\\\\\\"e\\\\\"");
}

static void test_shell_words_are_single_quoted(void) {
    CHECK_OWNED_STR(term_shell_quote("app"), "'app'");
    CHECK_OWNED_STR(term_shell_quote(""), "''");
    CHECK_OWNED_STR(term_shell_quote(NULL), "''");
    CHECK_OWNED_STR(term_shell_quote("o'brien; rm -rf /"), "'o'\\''brien; rm -rf /'");
    CHECK_OWNED_STR(term_shell_quote("$HOME `id`"), "'$HOME `id`'");
}

static void test_the_mysql_command_asks_for_the_password(void) {
    CHECK_OWNED_STR(term_mysql_command("127.0.0.1", 3306, "app"), "mysql --host='127.0.0.1' --port=3306 --user='app' --password");
    CHECK_OWNED_STR(term_mysql_command("db.internal", 3307, "it's"), "mysql --host='db.internal' --port=3307 --user='it'\\''s' --password");
}

void app_terminal_tests(void) {
    test_run("a shell session runs ssh to the server", test_a_shell_session_runs_ssh_to_the_server);
    test_run("a remote command gets a terminal and one quoted argument", test_a_remote_command_gets_a_terminal_and_one_quoted_argument);
    test_run("shell words are single-quoted", test_shell_words_are_single_quoted);
    test_run("the mysql command asks for the password", test_the_mysql_command_asks_for_the_password);
}
