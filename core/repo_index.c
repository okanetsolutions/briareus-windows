// A repository's files kept on this PC for the Files tab's searches: which files are worth keeping, the declarations
// found in them, the bytes on disk, bringing the index in line with a newer tree, and Go to Class, Go to Symbol and
// Find in Files over it.
#include "repo.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MARK: - What is indexed

static const char *const SKIPPED_FOLDERS[] = { "vendor", "node_modules", "bower_components", "dist", "build", "coverage", ".git", ".next", ".nuxt", ".cache", "target", NULL };
static const char *const SKIPPED_SUFFIXES[] = { ".min.js", ".min.css", ".map", ".lock", "-lock.json", "-lock.yaml", ".bundle.js", NULL };
static const char *const TEXT_EXTENSIONS[] = { "md", "markdown", "txt", "rst", NULL };

static bool in_list(const char *const *list, const char *word, size_t len) {
    for (; *list; list++) if (strlen(*list) == len && memcmp(*list, word, len) == 0) return true;
    return false;
}

bool repo_indexable(const char *path, long long size) {
    if (str_empty(path) || size > 1024 * 1024 || strpbrk(path, "\r\n")) return false;
    char *lower = str_fold(path);
    bool ok = true;
    // A folder of dependencies or build output anywhere above the file.
    for (const char *p = lower, *slash; ok && (slash = strchr(p, '/')); p = slash + 1) if (in_list(SKIPPED_FOLDERS, p, (size_t)(slash - p))) ok = false;
    for (const char *const *s = SKIPPED_SUFFIXES; ok && *s; s++) if (str_has_suffix(lower, *s)) ok = false;
    if (ok && str_eq(code_language_name(code_language_of(path)), "Text")) {
        const char *slash = strrchr(lower, '/'), *dot = strrchr(slash ? slash : lower, '.');
        ok = dot && in_list(TEXT_EXTENSIONS, dot + 1, strlen(dot + 1));
    }
    free(lower);
    return ok;
}

// MARK: - Declarations

static bool ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (unsigned char)c >= 0x80; }
static bool ident_char(char c) { return ident_start(c) || (c >= '0' && c <= '9'); }
static size_t skip_spaces(const char *s, size_t n, size_t i) { while (i < n && (s[i] == ' ' || s[i] == '\t')) i++; return i; }
static bool word_is(const char *s, size_t len, const char *word) { return strlen(word) == len && memcmp(s, word, len) == 0; }
static bool language_in(const char *language, const char *list) {
    // `list` names languages separated by "|".
    size_t n = strlen(language);
    for (const char *p = list; *p;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (len == n && memcmp(p, language, n) == 0) return true;
        p += len + (bar ? 1 : 0);
    }
    return false;
}

/// A word that declares what follows it, in the languages that have it (NULL: every language).
typedef struct { const char *word; SymbolKind kind; const char *languages; } Declarer;
static const Declarer DECLARERS[] = {
    { "class", SYMBOL_CLASS, NULL }, { "interface", SYMBOL_CLASS, NULL }, { "enum", SYMBOL_CLASS, NULL },
    { "trait", SYMBOL_CLASS, "PHP|Rust|Scala" }, { "struct", SYMBOL_CLASS, "C|C++|Objective-C|C#|Rust|Swift" },
    { "union", SYMBOL_CLASS, "C|C++" }, { "record", SYMBOL_CLASS, "Java|C#" }, { "protocol", SYMBOL_CLASS, "Swift" },
    { "object", SYMBOL_CLASS, "Kotlin|Scala" }, { "module", SYMBOL_CLASS, "Ruby" }, { "defmodule", SYMBOL_CLASS, "Elixir" },
    { "type", SYMBOL_CLASS, "TypeScript|Go" },
    { "function", SYMBOL_FUNCTION, "PHP|JavaScript|TypeScript|Lua|Shell" }, { "def", SYMBOL_FUNCTION, "Python|Ruby|Scala|Groovy|Elixir" },
    { "defp", SYMBOL_FUNCTION, "Elixir" }, { "fn", SYMBOL_FUNCTION, "Rust" }, { "func", SYMBOL_FUNCTION, "Go|Swift" },
    { "fun", SYMBOL_FUNCTION, "Kotlin" },
    { "const", SYMBOL_CONSTANT, "PHP|Go|Rust|JavaScript|TypeScript" },
};
/// Languages whose methods are declared without a keyword: `public void save(...) {`, `async load() {`.
static const char *const METHOD_LANGUAGES = "Java|C#|C|C++|Objective-C|Dart|JavaScript|TypeScript|Groovy";
/// Words before a parenthesis that are not a declaration's name, or start a statement that is not one.
static const char *const NOT_NAMES[] = {
    "if", "for", "while", "switch", "try", "catch", "return", "new", "else", "sizeof", "typeof", "function", "await", "throw", "yield",
    "case", "do", "delete", "in", "of", "using", "lock", "foreach", "synchronized", "super", "this", "assert", "with", "when",
    "match", "elif", "until", "unless", "defined", "decltype", "alignof", "static_assert", "operator", "echo", "print", NULL,
};
/// The words JavaScript and TypeScript allow before a method's name.
static const char *const JS_MODIFIERS[] = { "async", "static", "get", "set", "public", "private", "protected", "readonly", "abstract", "override", NULL };

typedef struct { RepoSymbol *items; size_t count, cap; } Symbols;
static void symbol_add(Symbols *s, const char *name, size_t len, const char *container, SymbolKind kind, int line) {
    if (s->count == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->items = xrealloc(s->items, s->cap * sizeof *s->items); }
    s->items[s->count++] = (RepoSymbol){ xstrndup(name, len), container ? xstrdup(container) : NULL, kind, 0, line };
}

/// The declarer `word` is in `language`, or NULL.
static const Declarer *declarer_of(const char *word, size_t len, const char *language) {
    for (size_t k = 0; k < sizeof DECLARERS / sizeof *DECLARERS; k++)
        if (word_is(word, len, DECLARERS[k].word) && (!DECLARERS[k].languages || language_in(language, DECLARERS[k].languages))) return &DECLARERS[k];
    return NULL;
}

/// The class the lines below are in: the last one declared, while lines are indented past it.
typedef struct { char *name; int indent; } Scope;

/// A declaration introduced by one of DECLARERS on a line, outside comments and strings; false when there is none.
static bool keyword_declaration(const char *code, size_t n, int indent, const char *language, size_t *name_at, size_t *name_len, SymbolKind *kind) {
    size_t first_word = skip_spaces(code, n, 0);
    for (size_t i = 0; i < n;) {
        if (!ident_start(code[i]) || (i && ident_char(code[i - 1]))) { i++; continue; }
        size_t j = i;
        while (j < n && ident_char(code[j])) j++;
        size_t before = i;
        while (before && (code[before - 1] == ' ' || code[before - 1] == '\t')) before--;
        char prev = before ? code[before - 1] : 0;
        const Declarer *d = declarer_of(code + i, j - i, language);
        // `Foo::class`, `$this->type`, `options.type` name a word; they declare nothing.
        if (!d || prev == '.' || prev == '>' || prev == ':' || prev == '$') { i = j; continue; }
        size_t k = skip_spaces(code, n, j);
        // PHP's `function &name` returns a reference; JavaScript's `function* name` is a generator.
        if (str_eq(d->word, "function") && k < n && (code[k] == '&' || code[k] == '*')) k = skip_spaces(code, n, k + 1);
        // Go's receiver: func (r *Repo) Name(.
        if (str_eq(d->word, "func") && k < n && code[k] == '(') {
            int depth = 0;
            for (; k < n; k++) { if (code[k] == '(') depth++; else if (code[k] == ')' && --depth == 0) { k++; break; } }
            k = skip_spaces(code, n, k);
        }
        if (k >= n || !ident_start(code[k]) || code[k] == '$') { i = j; continue; }
        size_t e = k;
        while (e < n && ident_char(code[e])) e++;
        // A function named through what holds it, Ruby's `def self.name` or Lua's `function M.name`: its last part.
        while (d->kind == SYMBOL_FUNCTION && e + 1 < n && (code[e] == '.' || code[e] == ':') && code[e + 1] != ':' && ident_start(code[e + 1])) { k = e + 1; e = k; while (e < n && ident_char(code[e])) e++; }
        // A class named through its namespace, Ruby's `class Admin::UsersController`: its last part too.
        while (d->kind == SYMBOL_CLASS && e + 2 < n && code[e] == ':' && code[e + 1] == ':' && ident_start(code[e + 2])) { k = e + 2; e = k; while (e < n && ident_char(code[e])) e++; }
        size_t after = skip_spaces(code, n, e);
        // A keyword in the name's place: Kotlin's `enum class Color` and `fun interface Runner` declare with the next
        // word, and Python's `from enum import`, or JavaScript's `class extends Base`, declare nothing here.
        if (word_is(code + k, e - k, "import") || word_is(code + k, e - k, "extends") || word_is(code + k, e - k, "implements") ||
            (declarer_of(code + k, e - k, language) && after < n && ident_start(code[after]))) { i = j; continue; }
        bool ok = true;
        // `type` and `module` lead their line, after `export` or `declare` at most.
        if (str_eq(d->word, "type") || str_eq(d->word, "module")) {
            size_t w = first_word;
            while (w < i) {
                size_t we = w; while (we < n && ident_char(code[we])) we++;
                if (!word_is(code + w, we - w, "export") && !word_is(code + w, we - w, "declare")) { ok = false; break; }
                w = skip_spaces(code, n, we);
            }
        }
        // In C a struct, union or enum is declared where a body or nothing follows its name; elsewhere it is a type in use.
        if (language_in(language, "C|C++|Objective-C") && (str_eq(d->word, "struct") || str_eq(d->word, "union") || str_eq(d->word, "enum")))
            ok = ok && (after >= n || code[after] == '{' || code[after] == ':');
        if (d->kind == SYMBOL_CONSTANT) {
            ok = ok && after < n && (code[after] == '=' || code[after] == ':');
            // JavaScript's const is a module's constant at the top level only; inside a function it is a local.
            if (language_in(language, "JavaScript|TypeScript")) ok = ok && indent == 0;
        }
        if (!ok) { i = j; continue; }
        *name_at = k; *name_len = e - k; *kind = d->kind;
        // A constant holding a function is one.
        if (d->kind == SYMBOL_CONSTANT) {
            const char *rest = code + after;
            size_t rn = n - after;
            for (size_t r = 0; r + 1 < rn; r++) if ((rest[r] == '=' && rest[r + 1] == '>') || (r + 8 <= rn && memcmp(rest + r, "function", 8) == 0)) { *kind = SYMBOL_FUNCTION; break; }
        }
        return true;
    }
    return false;
}

/// A method declared without a keyword: its name right before the first parenthesis, only modifiers and a type before
/// it, and a line that does not end as a statement does. `*container` is set for C++'s `Type::name(`. A line that starts
/// with `?` continues a ternary, `? foo(a)`, and declares nothing.
static bool method_declaration(const char *code, size_t n, const char *language, size_t *name_at, size_t *name_len, char **container) {
    size_t a = skip_spaces(code, n, 0), b = n;
    while (b > a && (code[b - 1] == ' ' || code[b - 1] == '\t')) b--;
    if (b <= a || code[b - 1] == ';' || code[a] == '?') return false;
    const char *paren = memchr(code + a, '(', b - a);
    if (!paren) return false;
    size_t p = (size_t)(paren - code), e = p;
    while (e > a && (code[e - 1] == ' ' || code[e - 1] == '\t')) e--;
    size_t s = e;
    while (s > a && ident_char(code[s - 1])) s--;
    // An annotation, @Get(...), names no method.
    if (s == e || !ident_start(code[s]) || in_list(NOT_NAMES, code + s, e - s) || (s > a && code[s - 1] == '@')) return false;
    // C++'s destructor ~Name.
    size_t prefix_end = s > a && code[s - 1] == '~' ? s - 1 : s;
    bool js = language_in(language, "JavaScript|TypeScript");
    char *scope = NULL;
    for (size_t i = a; i < prefix_end;) {
        char c = code[i];
        if (ident_start(c)) {
            size_t j = i; while (j < prefix_end && ident_char(code[j])) j++;
            bool first = i == a;
            if ((first && in_list(NOT_NAMES, code + i, j - i)) || (js && !in_list(JS_MODIFIERS, code + i, j - i))) { free(scope); return false; }
            // Type::name(
            if (j + 2 == prefix_end && code[j] == ':' && code[j + 1] == ':') { free(scope); scope = xstrndup(code + i, j - i); }
            i = j; continue;
        }
        if (c == ':' && i + 1 < prefix_end && code[i + 1] == ':') { i += 2; continue; }
        if (!strchr(" \t*&<>[],?@0123456789", c)) { free(scope); return false; }
        i++;
    }
    // The parameters close on the line, and a body, a return type or `throws` follows; or they run on to the next line.
    // A body opens at the line's end, or closes there too: `int getX() { return x; }`.
    // A call passing a callback, `describe('x', () => {`, leaves its parenthesis open with a body after it.
    size_t close = p;
    for (int depth = 0; close < b; close++) { if (code[close] == '(') depth++; else if (code[close] == ')' && --depth == 0) break; }
    char last = code[b - 1];
    bool declared;
    if (close < b) {
        size_t after = skip_spaces(code, b, close + 1);
        // C++'s `const`, `override` and `noexcept`, and Java's `throws A, B`, sit between the parameters and the body.
        if (prefix_end != a) while (after < b && (ident_char(code[after]) || code[after] == ',' || code[after] == '.' || code[after] == ' ')) after++;
        declared = after >= b ? prefix_end != a : (code[after] == '{' && (after == b - 1 || last == '}')) || code[after] == ':' || (b - after >= 6 && memcmp(code + after, "throws", 6) == 0);
    } else declared = prefix_end != a && (last == ',' || last == '(' || ident_char(last));
    if (!declared) { free(scope); return false; }
    *name_at = prefix_end; *name_len = e - prefix_end; *container = scope;
    return true;
}

/// C's anonymous `typedef struct {`, named by the `} Name;` that closes it: true when the line opens one.
static bool anonymous_typedef(const char *code, size_t n) {
    size_t i = skip_spaces(code, n, 0), e = i;
    while (e < n && ident_char(code[e])) e++;
    if (!word_is(code + i, e - i, "typedef")) return false;
    i = skip_spaces(code, n, e); e = i;
    while (e < n && ident_char(code[e])) e++;
    if (!word_is(code + i, e - i, "struct") && !word_is(code + i, e - i, "union") && !word_is(code + i, e - i, "enum")) return false;
    e = skip_spaces(code, n, e);
    return e >= n || code[e] == '{';
}

RepoSymbol *repo_symbols_of(const char *path, const char *content, size_t len, size_t *count) {
    *count = 0;
    const CodeLanguage *lang = code_language_of(path);
    const char *language = code_language_name(lang);
    if (!content || !len || language_in(language, "Text|Config|YAML|JSON|SQL|HTML|XML|Vue|Svelte|Blade|CSS|SCSS|Sass|Less|Makefile|Dockerfile")) return NULL;
    char *text = xstrndup(content, len);
    size_t line_count; RepoLine *lines = repo_lines(text, &line_count);
    CodeLexer lexer; code_lexer_init(&lexer, lang);
    Symbols out = { 0 };
    Scope scope = { NULL, -1 };
    bool methods = language_in(language, METHOD_LANGUAGES), c_like = language_in(language, "C|C++|Objective-C");
    // An anonymous typedef being read: the line it opens on, and how deep its braces are (-1: none).
    int typedef_line = 0, typedef_depth = -1;
    for (size_t i = 0; i < line_count; i++) {
        const char *line = text + lines[i].start;
        size_t n = lines[i].len, tn;
        // The line with its comments and strings blanked, so nothing in them reads as a declaration.
        CodeToken *tokens = code_lexer_line(&lexer, line, n, &tn);
        char *code = xstrndup(line, n);
        for (size_t t = 0; t < tn; t++) if (tokens[t].kind == CODE_COMMENT || tokens[t].kind == CODE_STRING) memset(code + tokens[t].start, ' ', tokens[t].len);
        free(tokens);
        int indent = 0;
        for (size_t k = 0; k < n && (code[k] == ' ' || code[k] == '\t'); k++) indent += code[k] == '\t' ? 4 : 1;
        size_t at, nlen; SymbolKind kind; char *owner = NULL;
        bool found = keyword_declaration(code, n, indent, language, &at, &nlen, &kind);
        if (!found && methods && method_declaration(code, n, language, &at, &nlen, &owner)) { found = true; kind = SYMBOL_FUNCTION; }
        int line_no = (int)i + 1;
        if (c_like && typedef_depth < 0 && anonymous_typedef(code, n)) { typedef_line = line_no; typedef_depth = 0; }
        for (size_t k = 0; typedef_depth >= 0 && k < n; k++) {
            if (code[k] == '{') typedef_depth++;
            else if (code[k] == '}' && --typedef_depth <= 0) {
                // The body closes: the first name after it, `} Name;` or `} Name, *NamePtr;`.
                typedef_depth = -1;
                size_t s = k + 1;
                while (s < n && (code[s] == ' ' || code[s] == '\t' || code[s] == '*')) s++;
                size_t e = s;
                while (e < n && ident_char(code[e])) e++;
                if (!found && e > s && ident_start(code[s])) { found = true; at = s; nlen = e - s; kind = SYMBOL_CLASS; line_no = typedef_line; }
            }
        }
        if (found) {
            // Leaving the class: a declaration as far left as it.
            if (scope.name && indent <= scope.indent) { free(scope.name); scope.name = NULL; scope.indent = -1; }
            symbol_add(&out, code + at, nlen, owner ? owner : scope.name, kind, line_no);
            if (kind == SYMBOL_CLASS) { free(scope.name); scope.name = xstrndup(code + at, nlen); scope.indent = indent; }
        }
        free(owner); free(code);
    }
    free(scope.name); free(lines); free(text);
    *count = out.count;
    return out.items;
}
void repo_symbols_free(RepoSymbol *symbols, size_t count) {
    for (size_t i = 0; i < count; i++) { free(symbols[i].name); free(symbols[i].container); }
    free(symbols);
}

// MARK: - The index

void repo_index_init(RepoIndex *index) { memset(index, 0, sizeof *index); }
static void index_file_free(RepoIndexFile *f) { free(f->path); free(f->content); }
void repo_index_free(RepoIndex *index) {
    for (size_t i = 0; i < index->count; i++) index_file_free(&index->files[i]);
    free(index->files); free(index->order);
    repo_symbols_free(index->symbols, index->symbol_count);
    free(index->ref); free(index->sha); free(index->partial);
    memset(index, 0, sizeof *index);
}
/// Where `path` is or would go in the order by path.
static size_t index_slot(const RepoIndex *index, const char *path, bool *found) {
    size_t lo = 0, hi = index->count;
    *found = false;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(index->files[index->order[mid]].path, path);
        if (!c) { *found = true; return mid; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
int repo_index_find(const RepoIndex *index, const char *path) {
    bool found = false;
    if (!path) return -1;
    size_t at = index_slot(index, path, &found);
    return found ? (int)index->order[at] : -1;
}
void repo_index_put(RepoIndex *index, const char *path, long long size, const char *content, size_t len) {
    bool found = false;
    size_t at = index_slot(index, path, &found), file;
    if (!found) {
        if (index->count == index->cap) {
            index->cap = index->cap ? index->cap * 2 : 64;
            index->files = xrealloc(index->files, index->cap * sizeof *index->files);
            index->order = xrealloc(index->order, index->cap * sizeof *index->order);
        }
        file = index->count;
        memmove(&index->order[at + 1], &index->order[at], (index->count - at) * sizeof *index->order);
        index->order[at] = file;
        index->count++;
        index->files[file].path = xstrdup(path);
    } else {
        file = index->order[at];
        free(index->files[file].content);
        index->bytes -= index->files[file].len;
    }
    index->files[file].size = size;
    index->files[file].content = xstrndup(content ? content : "", content ? len : 0);
    index->files[file].len = content ? len : 0;
    index->bytes += index->files[file].len;
}
bool repo_index_remove(RepoIndex *index, const char *path) {
    bool found = false;
    size_t at = path ? index_slot(index, path, &found) : 0;
    if (!found) return false;
    size_t file = index->order[at];
    index->bytes -= index->files[file].len;
    index_file_free(&index->files[file]);
    memmove(&index->files[file], &index->files[file + 1], (index->count - file - 1) * sizeof *index->files);
    memmove(&index->order[at], &index->order[at + 1], (index->count - at - 1) * sizeof *index->order);
    index->count--;
    for (size_t i = 0; i < index->count; i++) if (index->order[i] > file) index->order[i]--;
    return true;
}
void repo_index_symbols(RepoIndex *index) {
    repo_symbols_free(index->symbols, index->symbol_count);
    index->symbols = NULL; index->symbol_count = 0;
    size_t cap = 0;
    for (size_t k = 0; k < index->count; k++) {
        size_t i = index->order[k], n;
        RepoSymbol *found = repo_symbols_of(index->files[i].path, index->files[i].content, index->files[i].len, &n);
        if (index->symbol_count + n > cap) { cap = (index->symbol_count + n) * 2; index->symbols = xrealloc(index->symbols, cap * sizeof *index->symbols); }
        for (size_t j = 0; j < n; j++) { found[j].file = i; index->symbols[index->symbol_count++] = found[j]; }
        free(found);
    }
}

// MARK: - On disk
//
// "BRIAREUS-INDEX 1", then a `ref`, `sha` and `partial` line each (empty for none) and `files <n>`; then per file a
// line `<size> <length> <path>` and its bytes, each followed by a line break.

static const char MAGIC[] = "BRIAREUS-INDEX 1\n";

char *repo_index_serialize(const RepoIndex *index, size_t *len) {
    Str s; str_init(&s);
    str_appendz(&s, MAGIC);
    str_appendf(&s, "ref %s\nsha %s\npartial %s\nfiles %zu\n", index->ref ? index->ref : "", index->sha ? index->sha : "", index->partial ? index->partial : "", index->count);
    for (size_t i = 0; i < index->count; i++) {
        const RepoIndexFile *f = &index->files[index->order[i]];
        str_appendf(&s, "%lld %zu %s\n", f->size, f->len, f->path);
        str_append(&s, f->content, f->len);
        str_appendc(&s, '\n');
    }
    *len = s.len;
    return str_detach(&s);
}

/// The line at `*at` without its break, moving past it; NULL at the end.
static char *next_line(const char *data, size_t len, size_t *at) {
    if (*at >= len) return NULL;
    const char *nl = memchr(data + *at, '\n', len - *at);
    if (!nl) return NULL;
    char *line = xstrndup(data + *at, (size_t)(nl - data - *at));
    *at = (size_t)(nl - data) + 1;
    return line;
}
/// "<name> <value>": the value, NULL when empty; false for another name.
static bool header(const char *data, size_t len, size_t *at, const char *name, char **value) {
    char *line = next_line(data, len, at);
    size_t n = strlen(name);
    bool ok = line && strncmp(line, name, n) == 0 && line[n] == ' ';
    *value = ok && line[n + 1] ? xstrdup(line + n + 1) : NULL;
    free(line);
    return ok;
}
bool repo_index_parse(const char *data, size_t len, RepoIndex *out) {
    repo_index_init(out);
    size_t magic = sizeof MAGIC - 1, at = magic;
    if (!data || len < magic || memcmp(data, MAGIC, magic) != 0) return false;
    char *files = NULL;
    bool ok = header(data, len, &at, "ref", &out->ref) && header(data, len, &at, "sha", &out->sha) && header(data, len, &at, "partial", &out->partial)
              && header(data, len, &at, "files", &files) && files;
    char *end = NULL;
    unsigned long long count = ok ? strtoull(files, &end, 10) : 0;
    ok = ok && end && !*end;
    free(files);
    for (unsigned long long i = 0; ok && i < count; i++) {
        char *line = next_line(data, len, &at);
        long long size = 0; unsigned long long n = 0; int used = 0;
        ok = line && sscanf(line, "%lld %llu %n", &size, &n, &used) == 2 && used > 0 && line[used] && n <= len - at && at + n < len && data[at + n] == '\n';
        if (ok) { repo_index_put(out, line + used, size, data + at, (size_t)n); at += (size_t)n + 1; }
        free(line);
    }
    if (!ok) repo_index_free(out);
    return ok;
}

// MARK: - Keeping up with the tree

size_t repo_index_reconcile(RepoIndex *index, const RepoTree *tree, char *const *changed, size_t changed_count, char ***fetch) {
    for (size_t i = 0; i < changed_count; i++) repo_index_remove(index, changed[i]);
    for (size_t i = index->count; i-- > 0;) {
        int at = repo_tree_find(tree, index->files[i].path);
        if (at >= 0 && !tree->entries[at].folder && repo_indexable(tree->entries[at].path, tree->entries[at].size)) continue;
        char *path = xstrdup(index->files[i].path);
        repo_index_remove(index, path);
        free(path);
    }
    size_t n = 0, cap = 0, bytes = index->bytes;
    char **out = NULL;
    for (size_t i = 0; i < tree->count && index->count + n < REPO_INDEX_MAX_FILES; i++) {
        const RepoEntry *e = &tree->entries[i];
        if (e->folder || !repo_indexable(e->path, e->size) || repo_index_find(index, e->path) >= 0) continue;
        // A size the tree does not give counts for nothing here; the file is held to the limit once read.
        if (e->size > 0) { if (bytes > REPO_INDEX_MAX_BYTES || (unsigned long long)e->size > REPO_INDEX_MAX_BYTES - bytes) continue; bytes += (size_t)e->size; }
        if (n == cap) { cap = cap ? cap * 2 : 64; out = xrealloc(out, cap * sizeof *out); }
        out[n++] = xstrdup(e->path);
    }
    *fetch = out;
    return n;
}

bool repo_commit_changes(const Json *value, char ***paths, size_t *count, char **parent, bool *truncated) {
    *paths = NULL; *count = 0; *parent = NULL; *truncated = false;
    const Json *files = json_get(value, "files"), *commit = json_get(value, "commit");
    if (!json_is_object(commit) || !json_is_array(files)) return false;
    size_t n = 0;
    char **out = xcalloc(json_count(files) * 2 + 1, sizeof *out);
    for (size_t i = 0; i < json_count(files); i++) {
        const Json *f = json_at(files, i);
        const char *name = json_str_nonempty(json_get(f, "filename")), *before = json_str_nonempty(json_get(f, "previousFilename"));
        if (name) out[n++] = xstrdup(name);
        if (before) out[n++] = xstrdup(before);
    }
    *paths = out; *count = n;
    *parent = json_dup_str(json_at(json_get(commit, "parents"), 0));
    *truncated = json_bool_is(json_get(value, "truncated"), true);
    return true;
}

// MARK: - Searching

typedef struct { int score; size_t index, len; const char *name, *path; } Ranked;
static int ranked_compare(const void *x, const void *y) {
    const Ranked *a = x, *b = y;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    int c = strcmp(a->name, b->name);
    return c ? c : strcmp(a->path, b->path);
}
size_t *repo_index_find_symbols(const RepoIndex *index, const char *query, bool classes, size_t limit, size_t *count) {
    *count = 0;
    Str q; str_init(&q);
    for (const char *p = query ? query : ""; *p; p++) if (*p != ' ' && *p != '\t') str_appendc(&q, (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p));
    if (!q.len || !limit) { str_free(&q); return NULL; }
    Ranked *found = NULL; size_t n = 0, cap = 0;
    for (size_t i = 0; i < index->symbol_count; i++) {
        const RepoSymbol *s = &index->symbols[i];
        int score;
        if ((classes && s->kind != SYMBOL_CLASS) || !repo_match_score(q.data, s->name, &score)) continue;
        // The name itself, case ignored, before names that only hold the letters.
        if (str_ieq(s->name, q.data)) score += 500;
        if (n == cap) { cap = cap ? cap * 2 : 64; found = xrealloc(found, cap * sizeof *found); }
        found[n++] = (Ranked){ score, i, strlen(s->name), s->name, index->files[s->file].path };
    }
    str_free(&q);
    if (!n) { free(found); return NULL; }
    qsort(found, n, sizeof *found, ranked_compare);
    if (n > limit) n = limit;
    size_t *out = xcalloc(n, sizeof *out);
    for (size_t i = 0; i < n; i++) out[i] = found[i].index;
    free(found);
    *count = n;
    return out;
}

static char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }
RepoTextHit *repo_index_find_text(const RepoIndex *index, const char *query, size_t limit, size_t *count, size_t *total) {
    *count = 0; *total = 0;
    size_t qn = query ? strlen(query) : 0;
    if (!qn) return NULL;
    char *q = str_fold(query);
    RepoTextHit *out = NULL; size_t cap = 0;
    for (size_t o = 0; o < index->count; o++) {
        size_t f = index->order[o];
        const char *s = index->files[f].content;
        size_t n = index->files[f].len, line_start = 0;
        int line = 1;
        for (size_t i = 0; i + qn <= n; i++) {
            if (s[i] == '\n') { line++; line_start = i + 1; continue; }
            if (fold(s[i]) != q[0]) continue;
            size_t k = 1;
            while (k < qn && fold(s[i + k]) == q[k]) k++;
            if (k < qn) continue;
            (*total)++;
            if (*count < limit) {
                if (*count == cap) { cap = cap ? cap * 2 : 32; out = xrealloc(out, cap * sizeof *out); }
                out[(*count)++] = (RepoTextHit){ f, line, i - line_start, line_start };
            }
            // One hit per line: the rest of it is skipped.
            const char *nl = memchr(s + i, '\n', n - i);
            if (!nl) break;
            i = (size_t)(nl - s) - 1;
        }
    }
    free(q);
    return out;
}
