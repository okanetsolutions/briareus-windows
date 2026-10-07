// repo.c and highlight.c: the Files tab's tree order, Go to File, one file and its lines, and the colours of a line.
#include "json.h"
#include "repo.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static bool tree_of(const char *text, RepoTree *out) {
    Json *j = json_parsez(text);
    bool ok = repo_tree_parse(j, out);
    json_free(j);
    return ok;
}
/// The entries' paths in the order the tab lists them, joined with spaces.
static char *order_of(const RepoTree *t) {
    Str s; str_init(&s);
    for (size_t i = 0; i < t->count; i++) str_appendf(&s, "%s%s", i ? " " : "", t->entries[i].path);
    return str_detach(&s);
}

static void test_lists_folders_first_in_natural_order(void) {
    RepoTree t;
    CHECK(tree_of("{\"ref\":\"main\",\"sha\":\"abc\",\"truncated\":false,\"entries\":["
                  "{\"path\":\"src/file10.c\",\"type\":\"blob\",\"size\":10},{\"path\":\"README.md\",\"type\":\"blob\",\"size\":5},"
                  "{\"path\":\"src\",\"type\":\"tree\"},{\"path\":\"src/file2.c\",\"type\":\"blob\",\"size\":2},"
                  "{\"path\":\"src/lib\",\"type\":\"tree\"},{\"path\":\"src/lib/a.c\",\"type\":\"blob\"},"
                  "{\"path\":\"Makefile\",\"type\":\"blob\",\"size\":1},{\"path\":\"app\",\"type\":\"tree\"},{\"path\":\"\"},{\"type\":\"blob\"}]}", &t));
    CHECK_STR(t.ref, "main"); CHECK_STR(t.sha, "abc"); CHECK(!t.truncated);
    CHECK_OWNED_STR(order_of(&t), "app src src/lib src/lib/a.c src/file2.c src/file10.c Makefile README.md");
    CHECK_INT(t.count, 8);
    CHECK_INT(repo_tree_file_count(&t), 5);
    int lib = repo_tree_find(&t, "src/lib");
    CHECK(lib >= 0 && t.entries[lib].folder); CHECK_STR(t.entries[lib].name, "lib"); CHECK_INT(t.entries[lib].depth, 1);
    int a = repo_tree_find(&t, "src/lib/a.c");
    CHECK(a >= 0); CHECK_INT(t.entries[a].size, -1); CHECK_INT(t.entries[a].depth, 2);
    CHECK_INT(t.entries[repo_tree_find(&t, "src/file10.c")].size, 10);
    CHECK_INT(repo_tree_find(&t, "nope"), -1); CHECK_INT(repo_tree_find(&t, NULL), -1);
    // Skipping a folder steps over everything in it; a file is one row.
    int src = repo_tree_find(&t, "src");
    CHECK_INT(repo_tree_skip(&t, (size_t)src), repo_tree_find(&t, "Makefile"));
    CHECK_INT(repo_tree_skip(&t, (size_t)a), a + 1);
    CHECK_INT(repo_tree_skip(&t, t.count + 3), t.count);
    // Revealing a file unfolds the folders that hold it, and nothing else.
    repo_tree_reveal(&t, "src/lib/a.c");
    CHECK(t.entries[src].open); CHECK(t.entries[lib].open); CHECK(!t.entries[repo_tree_find(&t, "app")].open);
    repo_tree_reveal(&t, NULL);
    repo_tree_free(&t);
    CHECK_INT(t.count, 0);
}

static void test_puts_back_folders_a_truncated_list_skipped(void) {
    RepoTree t;
    CHECK(tree_of("{\"ref\":\"dev\",\"truncated\":true,\"entries\":[{\"path\":\"a/b/c.txt\",\"type\":\"blob\",\"size\":1},"
                  "{\"path\":\"a/b/c.txt\",\"type\":\"blob\"},{\"path\":\"z.txt\",\"type\":\"blob\"}]}", &t));
    CHECK(t.truncated); CHECK(!t.sha);
    CHECK_OWNED_STR(order_of(&t), "a a/b a/b/c.txt z.txt");
    CHECK(t.entries[0].folder); CHECK(t.entries[1].folder); CHECK_INT(t.entries[0].size, -1);
    repo_tree_free(&t);
    CHECK(!tree_of("{\"entries\":{}}", &t));
    CHECK(!tree_of("[]", &t));
    CHECK(tree_of("{\"entries\":[]}", &t));
    CHECK_INT(t.count, 0); CHECK_STR(t.ref, "");
    repo_tree_free(&t);
}

static void test_compares_names_as_people_do(void) {
    CHECK(repo_name_compare("file2", 5, "file10", 6) < 0);
    CHECK(repo_name_compare("File", 4, "file", 4) == 0);
    CHECK(repo_name_compare("a", 1, "B", 1) < 0);
    CHECK(repo_name_compare("v007", 4, "v7", 2) == 0);
    CHECK(repo_name_compare("v8", 2, "v007", 4) > 0);
    CHECK(repo_name_compare("abc", 3, "ab", 2) > 0);
    CHECK(repo_name_compare("x1y", 3, "x1z", 3) < 0);
    CHECK(repo_name_compare("12", 2, "13", 2) < 0);
}

static void test_finds_files_as_go_to_file_does(void) {
    RepoTree t;
    CHECK(tree_of("{\"entries\":[{\"path\":\"app/Http/Controllers/UserController.php\",\"type\":\"blob\"},"
                  "{\"path\":\"app/Models/User.php\",\"type\":\"blob\"},{\"path\":\"user/contracts/notes.md\",\"type\":\"blob\"},"
                  "{\"path\":\"app/Http\",\"type\":\"tree\"},{\"path\":\"tests/Feature/Http/UserControllerTest.php\",\"type\":\"blob\"}]}", &t));
    size_t n; size_t *found = repo_find_files(&t, "usercon", 10, &n);
    CHECK(n >= 2);
    if (n >= 2) {
        // Matched inside the file's name ranks above a match spread over folders; the shorter path wins a tie.
        CHECK_STR(t.entries[found[0]].path, "app/Http/Controllers/UserController.php");
        CHECK_STR(t.entries[found[1]].path, "tests/Feature/Http/UserControllerTest.php");
    }
    free(found);
    // Capitals as word starts, case and spaces ignored.
    found = repo_find_files(&t, "U C", 10, &n);
    CHECK(n >= 1); if (n) CHECK_STR(t.entries[found[0]].path, "app/Http/Controllers/UserController.php");
    free(found);
    // A slash matches across folders; folders are never found.
    found = repo_find_files(&t, "models/user", 10, &n);
    CHECK_INT(n, 1); if (n) CHECK_STR(t.entries[found[0]].path, "app/Models/User.php");
    free(found);
    CHECK(!repo_find_files(&t, "   ", 10, &n)); CHECK_INT(n, 0);
    CHECK(!repo_find_files(&t, NULL, 10, &n));
    CHECK(!repo_find_files(&t, "zzz", 10, &n));
    CHECK(!repo_find_files(&t, "user", 0, &n));
    found = repo_find_files(&t, "php", 1, &n);
    CHECK_INT(n, 1);
    free(found);
    int score;
    CHECK(!repo_match_score("", "a", &score));
    CHECK(!repo_match_score("a", NULL, &score));
    CHECK(repo_match_score("ab", "x/ab", &score)); CHECK(score > 1000);
    CHECK(repo_match_score("xa", "x/ab", &score)); CHECK(score < 1000);
    // A word start that leaves the rest of the query unmatched (the p of .php) falls back to a match inside a word.
    CHECK(repo_match_score("port", "app/Http/Report.php", &score)); CHECK(score > 1000);
    CHECK(repo_match_score("port", "ExportController.php", &score));
    CHECK(repo_match_score("pper", "src/Mapper.php", &score));
    CHECK(repo_match_score("pdate", "UpdateUser.php", &score));
    CHECK(repo_match_score("rco", "UserController_report.php", &score));
    CHECK(repo_match_score("ab", "Tab_a.txt", &score));
    CHECK(repo_match_score("ab", "TabA", &score));
    int mid;
    CHECK(repo_match_score("port", "app/Report.php", &mid));
    CHECK(repo_match_score("port", "app/Port.php", &score)); CHECK(score > mid);
    CHECK(!repo_match_score("port", "app/Pot.php", &score));
    repo_tree_free(&t);
}

static void test_reads_one_file(void) {
    Json *j = json_parsez("{\"path\":\"a/b.php\",\"ref\":\"abc\",\"size\":12,\"content\":\"<?php\\necho 1;\\n\",\"binary\":false,\"tooLarge\":false,\"url\":\"https://github.com/o/r/blob/abc/a/b.php\"}");
    RepoFile f;
    CHECK(repo_file_parse(j, &f));
    CHECK_STR(f.path, "a/b.php"); CHECK_STR(f.ref, "abc"); CHECK_INT(f.size, 12); CHECK_STR(f.content, "<?php\necho 1;\n");
    CHECK(!f.binary); CHECK(!f.too_large); CHECK_STR(f.url, "https://github.com/o/r/blob/abc/a/b.php");
    repo_file_free(&f);
    json_free(j);
    j = json_parsez("{\"path\":\"big.bin\",\"size\":2000000,\"content\":null,\"binary\":false,\"tooLarge\":true,\"url\":null}");
    CHECK(repo_file_parse(j, &f));
    CHECK(!f.content); CHECK(f.too_large); CHECK(!f.url); CHECK_STR(f.ref, "");
    repo_file_free(&f);
    json_free(j);
    j = json_parsez("{\"size\":1}");
    CHECK(!repo_file_parse(j, &f));
    json_free(j);
}

static void test_splits_lines(void) {
    size_t n;
    const char *text = "one\r\ntwo\n\nfour";
    RepoLine *lines = repo_lines(text, &n);
    CHECK_INT(n, 4);
    CHECK_INT(lines[0].len, 3); CHECK_INT(lines[1].start, 5); CHECK_INT(lines[1].len, 3); CHECK_INT(lines[2].len, 0);
    CHECK_INT(lines[3].start, 10); CHECK_INT(lines[3].len, 4);
    free(lines);
    // A final line break ends the last line instead of starting an empty one.
    lines = repo_lines("a\nb\r\n", &n); CHECK_INT(n, 2); CHECK_INT(lines[1].len, 1); free(lines);
    lines = repo_lines("x\r", &n); CHECK_INT(n, 1); CHECK_INT(lines[0].len, 1); free(lines);
    lines = repo_lines("", &n); CHECK_INT(n, 0); CHECK(lines != NULL); free(lines);
    lines = repo_lines(NULL, &n); CHECK_INT(n, 0); free(lines);
    // More lines than the first allocation holds.
    Str many; str_init(&many);
    for (int i = 0; i < 40; i++) str_appendz(&many, "l\n");
    str_appendz(&many, "last");
    lines = repo_lines(many.data, &n); CHECK_INT(n, 41); free(lines);
    str_free(&many);
}

/// A line's runs as "kind:text" joined with "|", the lexer's state carried from the calls before.
static char *runs_of(CodeLexer *lx, const char *line) {
    size_t n; CodeToken *t = code_lexer_line(lx, line, strlen(line), &n);
    static const char *const KINDS[] = { "p", "k", "s", "c", "n", "t", "v" };
    Str s; str_init(&s);
    size_t covered = 0;
    for (size_t i = 0; i < n; i++) { str_appendf(&s, "%s%s:%.*s", i ? "|" : "", KINDS[t[i].kind], (int)t[i].len, line + t[i].start); covered += t[i].len; }
    free(t);
    // Together the runs give the line back.
    CHECK_INT(covered, strlen(line));
    return str_detach(&s);
}

static void test_picks_a_language_by_name(void) {
    CHECK_STR(code_language_name(code_language_of("app/Models/User.php")), "PHP");
    CHECK_STR(code_language_name(code_language_of("resources/views/home.blade.php")), "Blade");
    CHECK_STR(code_language_name(code_language_of("Dockerfile")), "Dockerfile");
    CHECK_STR(code_language_name(code_language_of("deploy/.env")), "Config");
    CHECK_STR(code_language_name(code_language_of("src/App.TSX")), "TypeScript");
    CHECK_STR(code_language_name(code_language_of("core/repo.h")), "C");
    CHECK_STR(code_language_name(code_language_of("README.md")), "Text");
    CHECK_STR(code_language_name(code_language_of("LICENSE")), "Text");
    CHECK_STR(code_language_name(code_language_of(NULL)), "Text");
    CHECK_STR(code_language_name(NULL), "Text");
    CHECK_STR(code_language_name(code_language_of("a.less")), "Less");
}

static void test_colours_a_line(void) {
    CodeLexer lx;
    code_lexer_init(&lx, code_language_of("x.php"));
    CHECK_OWNED_STR(runs_of(&lx, "public function show($id) { return 'a\\'b' . 42; } // done"),
                    "k:public|p: |k:function|p: show(|v:$id|p:) { |k:return|p: |s:'a\\'b'|p: . |n:42|p:; } |c:// done");
    // A block comment runs into the next line, and ends there.
    CHECK_OWNED_STR(runs_of(&lx, "$x = new User(); /* one"), "v:$x|p: = |k:new|p: |t:User|p:(); |c:/* one");
    CHECK(lx.open_comment);
    CHECK_OWNED_STR(runs_of(&lx, "two */ #[Attr]"), "c:two */|p: |c:#[Attr]");
    CHECK(!lx.open_comment);
    CHECK_OWNED_STR(runs_of(&lx, ""), "");
    CHECK_OWNED_STR(runs_of(&lx, "/* still"), "c:/* still");
    CHECK_OWNED_STR(runs_of(&lx, "open"), "c:open");
    // A multi-line string, as Python's """.
    code_lexer_init(&lx, code_language_of("a.py"));
    CHECK_OWNED_STR(runs_of(&lx, "def f(): s = \"\"\"start"), "k:def|p: f(): s = |s:\"\"\"start");
    CHECK_OWNED_STR(runs_of(&lx, "middle"), "s:middle");
    CHECK_OWNED_STR(runs_of(&lx, "end\"\"\" # note"), "s:end\"\"\"|p: |c:# note");
    CHECK_OWNED_STR(runs_of(&lx, "x = \"\"\"one line\"\"\" + None"), "p:x = |s:\"\"\"one line\"\"\"|p: + |k:None");
    // An unclosed quote ends with its line; a word's "#" is not a comment in shell, and `${...}` is a variable.
    code_lexer_init(&lx, code_language_of("run.sh"));
    CHECK_OWNED_STR(runs_of(&lx, "echo ${HOME}/a#b \"open"), "k:echo|p: |v:${HOME}|p:/a#b |s:\"open");
    CHECK_OWNED_STR(runs_of(&lx, "x=$# # c"), "p:x=$# |c:# c");
    CHECK_OWNED_STR(runs_of(&lx, "echo ${unterminated"), "k:echo|p: |v:${unterminated");
    // SQL's keywords in any case; numbers inside words stay plain; hex numbers.
    code_lexer_init(&lx, code_language_of("q.sql"));
    CHECK_OWNED_STR(runs_of(&lx, "SELECT a1 FROM t WHERE x = 0x1F -- c"), "k:SELECT|p: a1 |k:FROM|p: t |k:WHERE|p: x = |n:0x1F|p: |c:-- c");
    // Ruby's defined?, C's preprocessor words, Lua's block comment.
    code_lexer_init(&lx, code_language_of("a.rb"));
    CHECK_OWNED_STR(runs_of(&lx, "defined?(x)"), "k:defined?|p:(x)");
    code_lexer_init(&lx, code_language_of("a.c"));
    CHECK_OWNED_STR(runs_of(&lx, "#include <stdio.h>"), "k:#include|p: <stdio.h>");
    code_lexer_init(&lx, code_language_of("a.lua"));
    CHECK_OWNED_STR(runs_of(&lx, "--[[ block ]] local x"), "c:--[[ block ]]|p: |k:local|p: x");
    // Plain text has no colours.
    code_lexer_init(&lx, NULL);
    CHECK_OWNED_STR(runs_of(&lx, "if \"x\" 1 # y"), "p:if \"x\" 1 # y");
    // A JavaScript template string across lines.
    code_lexer_init(&lx, code_language_of("a.js"));
    CHECK_OWNED_STR(runs_of(&lx, "const s = `a"), "k:const|p: s = |s:`a");
    CHECK_OWNED_STR(runs_of(&lx, "b` + ALL"), "s:b`|p: + ALL");
    // Go's raw strings have no escapes: a backslash before the closing backtick does not hide it.
    code_lexer_init(&lx, code_language_of("a.go"));
    CHECK_OWNED_STR(runs_of(&lx, "x := `\\` + y"), "p:x := |s:`\\`|p: + y");
    CHECK_OWNED_STR(runs_of(&lx, "s := `a"), "p:s := |s:`a");
    CHECK_OWNED_STR(runs_of(&lx, "b\\` + y"), "s:b\\`|p: + y");
}

void repo_tests(void) {
    test_run("repo lists folders first in natural order", test_lists_folders_first_in_natural_order);
    test_run("repo puts back folders a truncated list skipped", test_puts_back_folders_a_truncated_list_skipped);
    test_run("repo compares names as people do", test_compares_names_as_people_do);
    test_run("repo finds files as Go to File does", test_finds_files_as_go_to_file_does);
    test_run("repo reads one file", test_reads_one_file);
    test_run("repo splits lines", test_splits_lines);
    test_run("code picks a language by name", test_picks_a_language_by_name);
    test_run("code colours a line", test_colours_a_line);
}
