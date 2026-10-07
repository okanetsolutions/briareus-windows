// repo_index.c: which files are indexed, the declarations found in them, the index on disk, keeping up with a newer
// tree, and Go to Class, Go to Symbol and Find in Files.
#include "json.h"
#include "repo.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void test_picks_the_files_worth_indexing(void) {
    CHECK(repo_indexable("app/Models/User.php", 1200));
    CHECK(repo_indexable("README.md", 10));
    CHECK(repo_indexable("docs/notes.TXT", 10));
    CHECK(repo_indexable("src/index.ts", -1));
    CHECK(repo_indexable("Dockerfile", 10));
    CHECK(!repo_indexable("vendor/laravel/framework/src/Foo.php", 10));
    CHECK(!repo_indexable("web/node_modules/a/index.js", 10));
    CHECK(!repo_indexable("public/build/app.js", 10));
    CHECK(!repo_indexable("public/js/app.min.js", 10));
    CHECK(!repo_indexable("public/js/app.js.map", 10));
    CHECK(!repo_indexable("composer.lock", 10));
    CHECK(!repo_indexable("package-lock.json", 10));
    CHECK(!repo_indexable("big.php", 2 * 1024 * 1024));
    CHECK(!repo_indexable("logo.png", 10));
    CHECK(!repo_indexable("LICENSE", 10));
    CHECK(!repo_indexable("a\nb.php", 10));
    CHECK(!repo_indexable("", 10));
    CHECK(!repo_indexable(NULL, 10));
}

/// The declarations of a file as "kind:name@line" (with "<container" when in a class), joined with spaces.
static char *symbols_of(const char *path, const char *text) {
    size_t n; RepoSymbol *s = repo_symbols_of(path, text, strlen(text), &n);
    static const char KINDS[] = { 'c', 'f', 'k' };
    Str out; str_init(&out);
    for (size_t i = 0; i < n; i++) {
        str_appendf(&out, "%s%c:%s@%d", i ? " " : "", KINDS[s[i].kind], s[i].name, s[i].line);
        if (s[i].container) str_appendf(&out, "<%s", s[i].container);
    }
    repo_symbols_free(s, n);
    return str_detach(&out);
}

static void test_finds_declarations_in_php(void) {
    CHECK_OWNED_STR(symbols_of("app/User.php",
        "<?php\n"
        "namespace App;\n"
        "// class NotThis\n"
        "final class User extends Model implements HasName\n"
        "{\n"
        "    const ROLE = 'admin';\n"
        "    public static function &find($id) { return Foo::class; }\n"
        "    $s = 'function fake()';\n"
        "    private function save(): void\n"
        "}\n"
        "function helper() {}\n"
        "interface HasName {}\n"
        "trait Greets {}\n"
        "enum Status: string {}\n"),
        "c:User@4 k:ROLE@6<User f:find@7<User f:save@9<User f:helper@11 c:HasName@12 c:Greets@13 c:Status@14");
}

static void test_finds_declarations_in_javascript_and_typescript(void) {
    CHECK_OWNED_STR(symbols_of("src/a.ts",
        "export default class Store {\n"
        "  async load(id: string): Promise<void> {\n"
        "    const local = 1;\n"
        "    this.save(id);\n"
        "    if (x) {\n"
        "  }\n"
        "  static create() {\n"
        "}\n"
        "export const API_URL = 'x';\n"
        "export const handler = async (req) => {\n"
        "export type Id = string;\n"
        "interface Props {}\n"
        "describe('x', () => {\n"
        "function plain(a) {\n"
        "const options = { type: 'a' };\n"
        "function* saga() {\n"
        "export function *rootSaga() {}\n"),
        "c:Store@1 f:load@2<Store f:create@7<Store k:API_URL@9 f:handler@10 c:Id@11 c:Props@12 f:plain@14 k:options@15 f:saga@16 f:rootSaga@17");
}

static void test_finds_declarations_in_other_languages(void) {
    CHECK_OWNED_STR(symbols_of("a.py", "class Repo(Base):\n    def __init__(self):\n        pass\ndef main():\n    x = \"def no()\"\n"),
                    "c:Repo@1 f:__init__@2<Repo f:main@4");
    CHECK_OWNED_STR(symbols_of("a.go", "package main\ntype Server struct {\n}\nfunc (s *Server) Start() error {\nfunc main() {\nconst Limit = 3\n"),
                    "c:Server@2 f:Start@4 f:main@5 k:Limit@6");
    // A raw string ending in a backslash closes at its backtick, so the declarations after it are found.
    CHECK_OWNED_STR(symbols_of("a.go", "func Fix(p string) string {\n\treturn strings.ReplaceAll(p, `\\`, \"/\")\n}\nfunc Foo() {}\ntype Bar struct {}\n"),
                    "f:Fix@1 f:Foo@4 c:Bar@5");
    // Kotlin's and Scala's """ strings are raw too.
    CHECK_OWNED_STR(symbols_of("a.kt", "val p = \"\"\"C:\\\"\"\"\nfun later() {}\nclass Foo {}\n"), "f:later@2 c:Foo@3");
    CHECK_OWNED_STR(symbols_of("a.scala", "val p = \"\"\"C:\\\"\"\"\ndef later() = 1\nclass Foo {}\n"), "f:later@2 c:Foo@3");
    CHECK_OWNED_STR(symbols_of("a.rb", "module Billing\n  class Invoice\n    def self.build\n    def total\n"),
                    "c:Billing@1 c:Invoice@2<Billing f:build@3<Invoice f:total@4<Invoice");
    // A class named through its namespace is named by its last part.
    CHECK_OWNED_STR(symbols_of("a.rb", "class Admin::UsersController < ApplicationController\n  def index\nmodule Api::V1\n"),
                    "c:UsersController@1 f:index@2<UsersController c:V1@3");
    // Elixir's dotted module names too.
    CHECK_OWNED_STR(symbols_of("a.ex", "defmodule MyApp.Accounts.User do\n  def create(attrs) do\n"), "c:User@1 f:create@2<User");
    // A template's parameters are no classes; the class after them is.
    CHECK_OWNED_STR(symbols_of("a.cpp", "template <class T> class Vector {\n    int size() const {\ntemplate <class K, class V> struct Pair {\n"),
                    "c:Vector@1 f:size@2<Vector c:Pair@3");
    // A Go function literal is no declaration, whatever it returns.
    CHECK_OWNED_STR(symbols_of("a.go", "func run() {\n\tsort.Slice(xs, func(i, j int) bool {\n\tf := func(a int) error {\n"), "f:run@1");
    // Generic methods are named without their type parameters.
    CHECK_OWNED_STR(symbols_of("A.cs", "class A {\n    public T Get<T>(string key) {\n    var x = Make<int>(1);\n"), "c:A@1 f:Get@2<A");
    CHECK_OWNED_STR(symbols_of("a.ts", "class L {\n  map<T>(fn: (x: number) => T) {\n"), "c:L@1 f:map@2<L");
    // Calls continuing a list or an expression declare nothing.
    CHECK_OWNED_STR(symbols_of("a.dart", "class W {\n  Widget build(BuildContext context) {\n      const SizedBox(height: 8),\n"), "c:W@1 f:build@2<W");
    CHECK_OWNED_STR(symbols_of("build.gradle", "dependencies {\n    implementation project(':core')\n    implementation platform('x:y:1')\n"), "");
    CHECK_OWNED_STR(symbols_of("a.cpp", "Foo::Foo()\n    : m_foo(1)\n    , m_bar(2)\n{\n    std::cout << x\n        << compute(x)\n        && check(b)\n"), "f:Foo@1<Foo");
    // An anonymous typedef is named where its body closes; a named one where it opens.
    CHECK_OWNED_STR(symbols_of("a.h", "typedef struct {\n    union { int a; } u;\n    char *name;\n} RepoEntry;\ntypedef enum { A, B } Kind;\ntypedef struct Foo {\n} Foo;\nstruct Bar {\n};\ntypedef struct\n{\n} Pair, *PairPtr;\n"),
                    "c:RepoEntry@1 c:Kind@5 c:Foo@6 c:Bar@8 c:Pair@10");
    // A body that opens and closes on the declaration's line.
    CHECK_OWNED_STR(symbols_of("A.java", "class A {\n    public int getX() { return x; }\n    if (x) { y(); }\n"), "c:A@1 f:getX@2<A");
    CHECK_OWNED_STR(symbols_of("a.ts", "class S {\n  constructor(private readonly http: HttpClient) {}\n  get name() { return this._n; }\n"),
                    "c:S@1 f:constructor@2<S f:name@3<S");
    CHECK_OWNED_STR(symbols_of("a.c", "static int add(int a, int b) { return a + b; }\n"), "f:add@1");
    CHECK_OWNED_STR(symbols_of("a.c", "struct point {\n};\nstruct point *p = NULL;\nstatic int add(int a, int b)\n{\n    return add(a, b);\n}\nint main(void) {\n    if (x) {\n    foo(x)\n"),
                    "c:point@1 f:add@4 f:main@8");
    CHECK_OWNED_STR(symbols_of("a.cpp", "void Widget::draw(Canvas *cv) const {\n~Widget() {\n"), "f:draw@1<Widget f:~Widget@2");
    CHECK_OWNED_STR(symbols_of("A.java", "public class A {\n    @Override\n    @GetMapping(\"/x\")\n    public String name()\n    public void run() throws Exception {\n        String s = make(1);\n        return build(\n        new Thread() {\n"),
                    "c:A@1 f:name@4<A f:run@5<A");
    // Try-with-resources and a ternary's continuation line declare nothing.
    CHECK_OWNED_STR(symbols_of("A.java", "class A {\n    void run() {\n        try (Foo f = new Foo()) {\n        return c\n            ? compute(1)\n            : 0;\n"),
                    "c:A@1 f:run@2<A");
    CHECK_OWNED_STR(symbols_of("a.js", "class A {\n  pick() {\n    return cond\n      ? foo(a)\n      : bar(b);\n"), "c:A@1 f:pick@2<A");
    CHECK_OWNED_STR(symbols_of("a.rs", "pub struct Config {\npub fn load() -> Config {\nconst MAX: u32 = 1;\ntrait Shape {}\n"), "c:Config@1 f:load@2 k:MAX@3 c:Shape@4");
    CHECK_OWNED_STR(symbols_of("a.lua", "local function M.helper()\nfunction obj:method()\n"), "f:helper@1 f:method@2");
    CHECK_OWNED_STR(symbols_of("a.kt", "object Registry {\n    fun get() = 1\n"), "c:Registry@1 f:get@2<Registry");
    // A keyword where the name would be: the declaration is named by the word after it, or there is none.
    CHECK_OWNED_STR(symbols_of("a.kt", "enum class Color { RED }\nfun interface Runner {\n"), "c:Color@1 c:Runner@2");
    CHECK_OWNED_STR(symbols_of("a.cpp", "enum class Mode { On };\n"), "c:Mode@1");
    CHECK_OWNED_STR(symbols_of("a.py", "from enum import Enum\n"), "");
    CHECK_OWNED_STR(symbols_of("a.js", "export default class extends React.Component {\n  render() {\n"), "f:render@2");
    CHECK_OWNED_STR(symbols_of("a.ts", "const type = 'a';\n"), "k:type@1");
    // A default export without a semicolon calls a function; it declares none.
    CHECK_OWNED_STR(symbols_of("a.js", "export default withRouter(App)\n"), "");
    // Languages without declarations of their own, and nothing to read.
    CHECK_OWNED_STR(symbols_of("a.json", "{\"class\": 1}"), "");
    CHECK_OWNED_STR(symbols_of("README.md", "class Foo"), "");
    size_t n;
    CHECK(!repo_symbols_of("a.php", NULL, 0, &n)); CHECK_INT(n, 0);
}

static void test_keeps_files_in_order_and_on_disk(void) {
    RepoIndex x; repo_index_init(&x);
    repo_index_put(&x, "b.php", 5, "<?php", 5);
    repo_index_put(&x, "a/c.php", 20, "<?php\nclass C {}\n", 17);
    repo_index_put(&x, "b.php", 6, "<?php\nfunction b() {}\n", 22);
    repo_index_put(&x, "empty.txt", 0, NULL, 0);
    // Files keep the place they were added at; the order goes by path.
    CHECK_INT(x.count, 3);
    CHECK_STR(x.files[0].path, "b.php"); CHECK_STR(x.files[1].path, "a/c.php"); CHECK_INT(x.files[0].size, 6);
    CHECK_INT(x.order[0], 1); CHECK_INT(x.order[1], 0); CHECK_INT(x.order[2], 2);
    CHECK_INT(repo_index_find(&x, "b.php"), 0); CHECK_INT(repo_index_find(&x, "zz"), -1); CHECK_INT(repo_index_find(&x, NULL), -1);
    repo_index_symbols(&x);
    CHECK_INT(x.symbol_count, 2);
    CHECK_STR(x.symbols[0].name, "C"); CHECK_INT(x.symbols[0].file, 1);
    CHECK_STR(x.symbols[1].name, "b"); CHECK_INT(x.symbols[1].file, 0);
    x.ref = xstrdup("main"); x.sha = xstrdup("abc");
    size_t len; char *bytes = repo_index_serialize(&x, &len);
    RepoIndex y;
    CHECK(repo_index_parse(bytes, len, &y));
    CHECK_STR(y.ref, "main"); CHECK_STR(y.sha, "abc"); CHECK(!y.partial);
    CHECK_INT(y.count, 3);
    CHECK_STR(y.files[repo_index_find(&y, "b.php")].content, "<?php\nfunction b() {}\n");
    CHECK_INT(y.files[repo_index_find(&y, "b.php")].len, 22); CHECK_INT(y.files[repo_index_find(&y, "empty.txt")].len, 0);
    repo_index_free(&y);
    // Anything cut short or not written by the index is refused.
    CHECK(!repo_index_parse(bytes, len - 3, &y));
    CHECK(!repo_index_parse("nonsense", 8, &y));
    CHECK(!repo_index_parse(NULL, 0, &y));
    const char *bad = "BRIAREUS-INDEX 1\nref \nsha \npartial \nfiles x\n";
    CHECK(!repo_index_parse(bad, strlen(bad), &y));
    const char *missing = "BRIAREUS-INDEX 1\nref \nsha \nfiles 0\n";
    CHECK(!repo_index_parse(missing, strlen(missing), &y));
    const char *none = "BRIAREUS-INDEX 1\nref \nsha \npartial p1\nfiles 0\n";
    CHECK(repo_index_parse(none, strlen(none), &y)); CHECK(!y.ref); CHECK_STR(y.partial, "p1"); CHECK_INT(y.count, 0);
    repo_index_free(&y);
    free(bytes);
    // Removing one moves those after it.
    CHECK(repo_index_remove(&x, "b.php")); CHECK(!repo_index_remove(&x, "b.php")); CHECK(!repo_index_remove(&x, NULL));
    CHECK_INT(x.count, 2);
    CHECK_STR(x.files[0].path, "a/c.php"); CHECK_INT(repo_index_find(&x, "a/c.php"), 0); CHECK_INT(repo_index_find(&x, "empty.txt"), 1);
    repo_index_free(&x);
}

static void test_keeps_up_with_the_tree(void) {
    Json *j = json_parsez("{\"entries\":[{\"path\":\"a.php\",\"type\":\"blob\",\"size\":1},{\"path\":\"b.php\",\"type\":\"blob\",\"size\":1},"
                          "{\"path\":\"new.php\",\"type\":\"blob\",\"size\":1},{\"path\":\"logo.png\",\"type\":\"blob\",\"size\":1},"
                          "{\"path\":\"vendor/x.php\",\"type\":\"blob\",\"size\":1},{\"path\":\"src\",\"type\":\"tree\"}]}");
    RepoTree t; CHECK(repo_tree_parse(j, &t)); json_free(j);
    RepoIndex x; repo_index_init(&x);
    repo_index_put(&x, "a.php", 1, "a", 1);
    repo_index_put(&x, "b.php", 1, "b", 1);
    repo_index_put(&x, "gone.php", 1, "g", 1);
    char *changed[] = { "b.php" };
    char **fetch; size_t n = repo_index_reconcile(&x, &t, changed, 1, &fetch);
    // Gone and changed files leave; the changed one and the new one are read.
    CHECK_INT(x.count, 1); CHECK_STR(x.files[0].path, "a.php");
    CHECK_INT(n, 2);
    if (n == 2) { CHECK_STR(fetch[0], "b.php"); CHECK_STR(fetch[1], "new.php"); }
    str_array_free(fetch, n);
    repo_index_free(&x); repo_tree_free(&t);
}

static void test_holds_to_its_byte_limit(void) {
    RepoIndex x; repo_index_init(&x);
    repo_index_put(&x, "a.php", 1, "abc", 3);
    repo_index_put(&x, "b.php", 1, "de", 2);
    CHECK_INT(x.bytes, 5);
    repo_index_put(&x, "a.php", 1, "a", 1);
    CHECK_INT(x.bytes, 3);
    repo_index_remove(&x, "b.php");
    CHECK_INT(x.bytes, 1);
    // A tree of 1 MB files is read only as far as the limit allows, the index's own bytes counted.
    Str s; str_init(&s);
    str_appendz(&s, "{\"entries\":[");
    for (int i = 0; i < 300; i++) str_appendf(&s, "%s{\"path\":\"f%d.php\",\"type\":\"blob\",\"size\":1048576}", i ? "," : "", i);
    str_appendz(&s, ",{\"path\":\"a.php\",\"type\":\"blob\",\"size\":1},{\"path\":\"small.php\",\"type\":\"blob\",\"size\":-1}]}");
    Json *j = json_parse(s.data, s.len); str_free(&s);
    RepoTree t; CHECK(repo_tree_parse(j, &t)); json_free(j);
    char **fetch; size_t n = repo_index_reconcile(&x, &t, NULL, 0, &fetch);
    // 191 whole megabytes fit beside a.php's byte; the file of unknown size is read and held to the limit then.
    CHECK_INT(n, 192);
    if (n == 192) CHECK_STR(fetch[191], "small.php");
    str_array_free(fetch, n);
    repo_index_free(&x); repo_tree_free(&t);
}

static void test_reads_a_commit_for_walking_back(void) {
    Json *j = json_parsez("{\"commit\":{\"sha\":\"c2\",\"parents\":[\"c1\",\"m\"]},\"files\":[{\"filename\":\"a.php\"},"
                          "{\"filename\":\"new.php\",\"previousFilename\":\"old.php\"},{\"status\":\"x\"}],\"truncated\":true}");
    char **paths; size_t n; char *parent; bool truncated;
    CHECK(repo_commit_changes(j, &paths, &n, &parent, &truncated));
    CHECK_INT(n, 3); CHECK_STR(paths[0], "a.php"); CHECK_STR(paths[1], "new.php"); CHECK_STR(paths[2], "old.php");
    CHECK_STR(parent, "c1"); CHECK(truncated);
    str_array_free(paths, n); free(parent);
    json_free(j);
    j = json_parsez("{\"commit\":{\"sha\":\"root\",\"parents\":[]},\"files\":[]}");
    CHECK(repo_commit_changes(j, &paths, &n, &parent, &truncated));
    CHECK_INT(n, 0); CHECK(!parent); CHECK(!truncated);
    str_array_free(paths, n);
    json_free(j);
    j = json_parsez("{\"files\":[]}");
    CHECK(!repo_commit_changes(j, &paths, &n, &parent, &truncated));
    json_free(j);
}

static void test_searches_symbols_and_text(void) {
    RepoIndex x; repo_index_init(&x);
    const char *user = "<?php\nclass UserController {\n    public function show() {}\n}\n";
    const char *repo = "<?php\nclass UserRepository {\n    public function findUser() {}\n}\nfunction user() {}\n";
    repo_index_put(&x, "app/Http/UserController.php", 1, user, strlen(user));
    repo_index_put(&x, "app/UserRepository.php", 1, repo, strlen(repo));
    repo_index_symbols(&x);
    size_t n; size_t *found = repo_index_find_symbols(&x, "UC", true, 10, &n);
    CHECK_INT(n, 1); if (n) CHECK_STR(x.symbols[found[0]].name, "UserController");
    free(found);
    // The name itself first; classes alone leave the functions out.
    found = repo_index_find_symbols(&x, "user", false, 10, &n);
    CHECK(n >= 3); if (n) CHECK_STR(x.symbols[found[0]].name, "user");
    free(found);
    found = repo_index_find_symbols(&x, "user", true, 10, &n);
    CHECK_INT(n, 2);
    free(found);
    found = repo_index_find_symbols(&x, "user", false, 1, &n); CHECK_INT(n, 1); free(found);
    CHECK(!repo_index_find_symbols(&x, " ", false, 10, &n));
    CHECK(!repo_index_find_symbols(&x, "zzz", false, 10, &n));
    CHECK(!repo_index_find_symbols(&x, "a", false, 0, &n));
    size_t total; RepoTextHit *hits = repo_index_find_text(&x, "FUNCTION", 2, &n, &total);
    // Case ignored, one hit per line, files in path order; past the limit only counted.
    CHECK_INT(total, 3); CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_STR(x.files[hits[0].file].path, "app/Http/UserController.php"); CHECK_INT(hits[0].line, 3); CHECK_INT(hits[0].column, 11);
        CHECK_INT(hits[0].start, 29);
        CHECK_STR(x.files[hits[1].file].path, "app/UserRepository.php"); CHECK_INT(hits[1].line, 3);
    }
    free(hits);
    hits = repo_index_find_text(&x, "class user", 10, &n, &total);
    CHECK_INT(n, 2); free(hits);
    CHECK(!repo_index_find_text(&x, "", 10, &n, &total)); CHECK_INT(total, 0);
    CHECK(!repo_index_find_text(&x, NULL, 10, &n, &total));
    CHECK(!repo_index_find_text(&x, "nowhere", 10, &n, &total));
    repo_index_free(&x);
}

void repo_index_tests(void) {
    test_run("index picks the files worth indexing", test_picks_the_files_worth_indexing);
    test_run("index finds declarations in PHP", test_finds_declarations_in_php);
    test_run("index finds declarations in JavaScript and TypeScript", test_finds_declarations_in_javascript_and_typescript);
    test_run("index finds declarations in other languages", test_finds_declarations_in_other_languages);
    test_run("index keeps files in order and on disk", test_keeps_files_in_order_and_on_disk);
    test_run("index keeps up with the tree", test_keeps_up_with_the_tree);
    test_run("index holds to its byte limit", test_holds_to_its_byte_limit);
    test_run("index reads a commit for walking back", test_reads_a_commit_for_walking_back);
    test_run("index searches symbols and text", test_searches_symbols_and_text);
}
