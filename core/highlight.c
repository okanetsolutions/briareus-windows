// The colours of a source file in the Files tab: a small lexer per family of languages, picked by the file's name, that
// marks comments, strings, numbers, keywords and type names. It reads a file line after line, carrying a block comment
// or a multi-line string from one line into the next; what it does not know stays plain. The Mac app's CodeHighlight.
#include "repo.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

struct CodeLanguage {
    const char *name;
    const char *line_comments[3];            // up to two, NULL-terminated
    const char *block_open, *block_close;    // NULL for none
    const char *quotes;                      // the quotes that open a string ending on the same line
    const char *multiline[3];                // delimiters of strings that may run across lines: """ in Python, ` in JavaScript
    const char *const *keywords[2];          // NULL-terminated lists
    bool keywords_ci;                        // keywords in any case (SQL)
    bool dollar;                             // `$name` reads as a variable (PHP, shell)
    bool types;                              // capitalised words read as type names
};

// MARK: - Keywords

static const char *const SWIFT[] = { "actor", "any", "as", "associatedtype", "async", "await", "break", "case", "catch", "class", "continue", "default", "defer", "deinit", "do", "else", "enum", "extension", "fallthrough", "false", "fileprivate", "final", "for", "func", "guard", "if", "import", "in", "init", "inout", "internal", "is", "lazy", "let", "mutating", "nil", "nonisolated", "open", "operator", "override", "private", "protocol", "public", "repeat", "rethrows", "return", "self", "Self", "some", "static", "struct", "subscript", "super", "switch", "throw", "throws", "true", "try", "typealias", "var", "weak", "where", "while", "willSet", "didSet", "get", "set", NULL };
static const char *const PHP[] = { "abstract", "and", "array", "as", "break", "callable", "case", "catch", "class", "clone", "const", "continue", "declare", "default", "do", "echo", "else", "elseif", "empty", "enum", "extends", "false", "final", "finally", "fn", "for", "foreach", "function", "global", "if", "implements", "include", "include_once", "instanceof", "insteadof", "interface", "isset", "list", "match", "namespace", "new", "null", "or", "print", "private", "protected", "public", "readonly", "require", "require_once", "return", "self", "static", "parent", "switch", "throw", "trait", "true", "try", "unset", "use", "var", "while", "xor", "yield", "int", "string", "bool", "float", "void", "mixed", "never", "object", "iterable", NULL };
static const char *const JAVASCRIPT[] = { "async", "await", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do", "else", "export", "extends", "false", "finally", "for", "from", "function", "get", "if", "import", "in", "instanceof", "let", "new", "null", "of", "return", "set", "static", "super", "switch", "this", "throw", "true", "try", "typeof", "undefined", "var", "void", "while", "with", "yield", NULL };
static const char *const TYPESCRIPT[] = { "abstract", "any", "as", "boolean", "declare", "enum", "implements", "interface", "keyof", "namespace", "never", "number", "private", "protected", "public", "readonly", "satisfies", "string", "type", "unknown", NULL };
static const char *const JAVA[] = { "abstract", "assert", "boolean", "break", "byte", "case", "catch", "char", "class", "const", "continue", "default", "do", "double", "else", "enum", "extends", "false", "final", "finally", "float", "for", "if", "implements", "import", "instanceof", "int", "interface", "long", "new", "null", "package", "private", "protected", "public", "record", "return", "short", "static", "super", "switch", "synchronized", "this", "throw", "throws", "true", "try", "var", "void", "volatile", "while", NULL };
static const char *const KOTLIN[] = { "as", "break", "class", "companion", "continue", "data", "do", "else", "enum", "false", "for", "fun", "if", "import", "in", "interface", "internal", "is", "lateinit", "null", "object", "open", "override", "package", "private", "protected", "public", "return", "sealed", "super", "suspend", "this", "throw", "true", "try", "typealias", "val", "var", "when", "while", "def", "new", NULL };
static const char *const GO[] = { "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough", "false", "for", "func", "go", "goto", "if", "import", "interface", "iota", "map", "nil", "package", "range", "return", "select", "struct", "switch", "true", "type", "var", NULL };
static const char *const RUST[] = { "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self", "static", "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while", NULL };
static const char *const C_WORDS[] = { "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else", "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while", "NULL", "true", "false", "bool", "#include", "#define", "#ifdef", "#ifndef", "#endif", "#if", "#else", "#pragma", NULL };
static const char *const CPP[] = { "class", "namespace", "template", "typename", "public", "private", "protected", "virtual", "override", "new", "delete", "this", "nullptr", "using", "try", "catch", "throw", "constexpr", "auto", "noexcept", NULL };
static const char *const OBJC[] = { "@interface", "@implementation", "@end", "@property", "@protocol", "@class", "self", "nil", "YES", "NO", "id", NULL };
static const char *const CSHARP[] = { "abstract", "as", "async", "await", "base", "bool", "break", "case", "catch", "class", "const", "continue", "default", "do", "else", "enum", "false", "finally", "for", "foreach", "if", "in", "int", "interface", "internal", "is", "namespace", "new", "null", "out", "override", "private", "protected", "public", "readonly", "record", "ref", "return", "sealed", "static", "string", "struct", "switch", "this", "throw", "true", "try", "using", "var", "virtual", "void", "while", NULL };
static const char *const DART[] = { "abstract", "as", "async", "await", "break", "case", "catch", "class", "const", "continue", "default", "do", "else", "enum", "extends", "false", "final", "finally", "for", "if", "implements", "import", "in", "is", "late", "new", "null", "required", "return", "super", "switch", "this", "throw", "true", "try", "var", "void", "while", "with", NULL };
static const char *const PYTHON[] = { "and", "as", "assert", "async", "await", "break", "class", "continue", "def", "del", "elif", "else", "except", "False", "finally", "for", "from", "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not", "or", "pass", "raise", "return", "self", "True", "try", "while", "with", "yield", NULL };
static const char *const RUBY[] = { "alias", "and", "begin", "break", "case", "class", "def", "defined?", "do", "else", "elsif", "end", "ensure", "false", "for", "if", "in", "module", "next", "nil", "not", "or", "redo", "rescue", "retry", "return", "self", "super", "then", "true", "undef", "unless", "until", "when", "while", "yield", "require", "attr_accessor", "attr_reader", NULL };
static const char *const SHELL[] = { "if", "then", "else", "elif", "fi", "case", "esac", "for", "while", "until", "do", "done", "in", "function", "return", "export", "local", "readonly", "set", "unset", "echo", "exit", "source", "true", "false", NULL };
static const char *const DOCKER[] = { "from", "run", "cmd", "copy", "add", "env", "arg", "workdir", "expose", "entrypoint", "user", "volume", "label", "as", NULL };
static const char *const SQL[] = { "select", "from", "where", "and", "or", "not", "insert", "into", "values", "update", "set", "delete", "create", "table", "alter", "drop", "index", "primary", "key", "foreign", "references", "join", "left", "right", "inner", "outer", "on", "as", "group", "by", "order", "having", "limit", "offset", "null", "is", "in", "like", "distinct", "union", "all", "case", "when", "then", "else", "end", "default", "unique", "constraint", "exists", "if", "begin", "commit", "rollback", "int", "varchar", "text", "boolean", "timestamp", "true", "false", NULL };
static const char *const CSS[] = { "!important", "@media", "@import", "@keyframes", "@font-face", "@supports", "@apply", "@tailwind", "@layer", "@use", "@mixin", "@include", NULL };
static const char *const LUA[] = { "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while", NULL };
static const char *const ELIXIR[] = { "def", "defp", "defmodule", "do", "end", "fn", "if", "else", "case", "cond", "with", "true", "false", "nil", "when", "import", "alias", "use", "require", NULL };
static const char *const YAML[] = { "true", "false", "null", "yes", "no", "on", "off", NULL };
static const char *const CONFIG[] = { "true", "false", NULL };
static const char *const JSON_WORDS[] = { "true", "false", "null", NULL };

// MARK: - Languages

#define QUOTES "\"'"
#define C_LIKE(n, k1, k2, q, lc2, dollar_, ...) { n, { "//", lc2, NULL }, "/*", "*/", q, { __VA_ARGS__ }, { k1, k2 }, false, dollar_, true }
static const CodeLanguage PLAIN = { "Text", { NULL }, NULL, NULL, "", { NULL }, { NULL, NULL }, false, false, false };
static const CodeLanguage LANGUAGES[] = {
    C_LIKE("Swift", SWIFT, NULL, "\"", NULL, false, "\"\"\"", NULL),
    C_LIKE("PHP", PHP, NULL, QUOTES, "#", true, NULL),
    C_LIKE("JavaScript", JAVASCRIPT, NULL, QUOTES, NULL, false, "`", NULL),
    C_LIKE("TypeScript", JAVASCRIPT, TYPESCRIPT, QUOTES, NULL, false, "`", NULL),
    C_LIKE("Java", JAVA, NULL, QUOTES, NULL, false, "\"\"\"", NULL),
    C_LIKE("Kotlin", KOTLIN, NULL, QUOTES, NULL, false, "\"\"\"", NULL),
    C_LIKE("Go", GO, NULL, QUOTES, NULL, false, "`", NULL),
    C_LIKE("Rust", RUST, NULL, "\"", NULL, false, NULL),
    C_LIKE("C", C_WORDS, NULL, QUOTES, NULL, false, NULL),
    C_LIKE("C++", C_WORDS, CPP, QUOTES, NULL, false, NULL),
    C_LIKE("Objective-C", C_WORDS, OBJC, QUOTES, NULL, false, NULL),
    C_LIKE("C#", CSHARP, NULL, QUOTES, NULL, false, NULL),
    C_LIKE("Dart", DART, NULL, QUOTES, NULL, false, "'''", "\"\"\"", NULL),
    C_LIKE("Scala", KOTLIN, NULL, QUOTES, NULL, false, "\"\"\"", NULL),
    C_LIKE("Groovy", KOTLIN, JAVA, QUOTES, NULL, false, "\"\"\"", "'''", NULL),
    { "Python", { "#", NULL }, NULL, NULL, QUOTES, { "\"\"\"", "'''", NULL }, { PYTHON, NULL }, false, false, true },
    { "Ruby", { "#", NULL }, NULL, NULL, QUOTES, { NULL }, { RUBY, NULL }, false, false, true },
    { "Shell", { "#", NULL }, NULL, NULL, QUOTES, { NULL }, { SHELL, NULL }, false, true, false },
    { "Dockerfile", { "#", NULL }, NULL, NULL, QUOTES, { NULL }, { SHELL, DOCKER }, true, true, false },
    { "Makefile", { "#", NULL }, NULL, NULL, QUOTES, { NULL }, { SHELL, NULL }, false, true, false },
    { "YAML", { "#", NULL }, NULL, NULL, QUOTES, { NULL }, { YAML, NULL }, false, false, false },
    { "Config", { "#", ";", NULL }, NULL, NULL, QUOTES, { NULL }, { CONFIG, NULL }, false, false, false },
    { "JSON", { "//", NULL }, "/*", "*/", "\"", { NULL }, { JSON_WORDS, NULL }, false, false, false },
    { "SQL", { "--", "#", NULL }, "/*", "*/", "'\"`", { NULL }, { SQL, NULL }, true, false, false },
    { "HTML", { NULL }, "<!--", "-->", QUOTES, { NULL }, { NULL, NULL }, false, false, false },
    { "XML", { NULL }, "<!--", "-->", QUOTES, { NULL }, { NULL, NULL }, false, false, false },
    { "Vue", { NULL }, "<!--", "-->", QUOTES, { NULL }, { NULL, NULL }, false, false, false },
    { "Svelte", { NULL }, "<!--", "-->", QUOTES, { NULL }, { NULL, NULL }, false, false, false },
    { "Blade", { NULL }, "<!--", "-->", QUOTES, { NULL }, { NULL, NULL }, false, false, false },
    { "CSS", { NULL }, "/*", "*/", QUOTES, { NULL }, { CSS, NULL }, false, false, false },
    { "SCSS", { "//", NULL }, "/*", "*/", QUOTES, { NULL }, { CSS, NULL }, false, false, false },
    { "Sass", { "//", NULL }, "/*", "*/", QUOTES, { NULL }, { CSS, NULL }, false, false, false },
    { "Less", { "//", NULL }, "/*", "*/", QUOTES, { NULL }, { CSS, NULL }, false, false, false },
    { "Lua", { "--", NULL }, "--[[", "]]", QUOTES, { NULL }, { LUA, NULL }, false, false, true },
    { "Elixir", { "#", NULL }, NULL, NULL, QUOTES, { "\"\"\"", NULL }, { ELIXIR, NULL }, false, false, true },
};

static const CodeLanguage *language_named(const char *name) {
    for (size_t i = 0; i < sizeof LANGUAGES / sizeof *LANGUAGES; i++) if (str_eq(LANGUAGES[i].name, name)) return &LANGUAGES[i];
    return &PLAIN;
}

const CodeLanguage *code_language_of(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    char *name = str_fold(slash ? slash + 1 : path ? path : "");
    const char *language = NULL;
    static const struct { const char *name, *language; } NAMES[] = {
        { "dockerfile", "Dockerfile" }, { "containerfile", "Dockerfile" }, { "makefile", "Makefile" }, { "gnumakefile", "Makefile" },
        { ".env", "Config" }, { ".gitignore", "Config" }, { ".dockerignore", "Config" }, { ".editorconfig", "Config" }, { ".npmrc", "Config" },
    };
    static const struct { const char *ext, *language; } EXTENSIONS[] = {
        { "swift", "Swift" }, { "php", "PHP" }, { "phtml", "PHP" },
        { "js", "JavaScript" }, { "mjs", "JavaScript" }, { "cjs", "JavaScript" }, { "jsx", "JavaScript" },
        { "ts", "TypeScript" }, { "mts", "TypeScript" }, { "cts", "TypeScript" }, { "tsx", "TypeScript" },
        { "java", "Java" }, { "kt", "Kotlin" }, { "kts", "Kotlin" }, { "go", "Go" }, { "rs", "Rust" },
        { "c", "C" }, { "h", "C" }, { "cc", "C++" }, { "cpp", "C++" }, { "cxx", "C++" }, { "hpp", "C++" }, { "hh", "C++" }, { "hxx", "C++" },
        { "m", "Objective-C" }, { "mm", "Objective-C" }, { "cs", "C#" }, { "dart", "Dart" }, { "scala", "Scala" },
        { "gradle", "Groovy" }, { "groovy", "Groovy" }, { "py", "Python" }, { "pyi", "Python" },
        { "rb", "Ruby" }, { "rake", "Ruby" }, { "gemspec", "Ruby" },
        { "sh", "Shell" }, { "bash", "Shell" }, { "zsh", "Shell" }, { "fish", "Shell" }, { "command", "Shell" },
        { "yml", "YAML" }, { "yaml", "YAML" },
        { "toml", "Config" }, { "ini", "Config" }, { "cfg", "Config" }, { "conf", "Config" }, { "properties", "Config" },
        { "json", "JSON" }, { "jsonc", "JSON" }, { "json5", "JSON" }, { "lock", "JSON" }, { "sql", "SQL" },
        { "html", "HTML" }, { "htm", "HTML" }, { "xml", "XML" }, { "svg", "XML" }, { "plist", "XML" }, { "xib", "XML" }, { "storyboard", "XML" },
        { "vue", "Vue" }, { "svelte", "Svelte" }, { "css", "CSS" }, { "scss", "SCSS" }, { "sass", "Sass" }, { "less", "Less" },
        { "lua", "Lua" }, { "ex", "Elixir" }, { "exs", "Elixir" },
    };
    for (size_t i = 0; !language && i < sizeof NAMES / sizeof *NAMES; i++) if (str_eq(name, NAMES[i].name)) language = NAMES[i].language;
    if (!language && str_has_suffix(name, ".blade.php")) language = "Blade";
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; !language && dot && i < sizeof EXTENSIONS / sizeof *EXTENSIONS; i++) if (str_eq(dot + 1, EXTENSIONS[i].ext)) language = EXTENSIONS[i].language;
    free(name);
    return language ? language_named(language) : &PLAIN;
}
const char *code_language_name(const CodeLanguage *language) { return language ? language->name : PLAIN.name; }

// MARK: - Lexer

static bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (unsigned char)c >= 0x80; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_word_char(char c) { return is_letter(c) || is_digit(c) || c == '_'; }
static bool is_word_start(char c) { return is_letter(c) || c == '_' || c == '@' || c == '#'; }
static char fold(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static bool matches(const char *token, const char *s, size_t n, size_t i) {
    size_t len = strlen(token);
    return len && i + len <= n && memcmp(s + i, token, len) == 0;
}
/// Where `token` ends (the index after it) from `from` on, a backslash escaping the character after it when `escapes`;
/// `n` + 1 when it is not there.
static size_t find(const char *token, const char *s, size_t n, size_t from, bool escapes) {
    for (size_t i = from; i < n;) {
        if (escapes && s[i] == '\\') { i += 2; continue; }
        if (matches(token, s, n, i)) return i + strlen(token);
        i++;
    }
    return n + 1;
}
static bool is_keyword(const CodeLanguage *l, const char *word, size_t len) {
    for (int k = 0; k < 2; k++) {
        for (const char *const *w = l->keywords[k]; w && *w; w++) {
            if (strlen(*w) != len) continue;
            if (!l->keywords_ci) { if (memcmp(*w, word, len) == 0) return true; continue; }
            size_t i = 0;
            while (i < len && fold((*w)[i]) == fold(word[i])) i++;
            if (i == len) return true;
        }
    }
    return false;
}

typedef struct { CodeToken *items; size_t count, cap; } Tokens;
/// Adds [start, end) as `kind`, joined to the run before when it is the same kind.
static void emit(Tokens *t, size_t start, size_t end, CodeKind kind) {
    if (end <= start) return;
    if (t->count && t->items[t->count - 1].kind == kind && t->items[t->count - 1].start + t->items[t->count - 1].len == start) { t->items[t->count - 1].len += end - start; return; }
    if (t->count == t->cap) { t->cap = t->cap ? t->cap * 2 : 8; t->items = xrealloc(t->items, t->cap * sizeof *t->items); }
    t->items[t->count++] = (CodeToken){ start, end - start, kind };
}

void code_lexer_init(CodeLexer *lexer, const CodeLanguage *language) {
    memset(lexer, 0, sizeof *lexer);
    lexer->language = language ? language : &PLAIN;
}

CodeToken *code_lexer_line(CodeLexer *lexer, const char *s, size_t n, size_t *count) {
    const CodeLanguage *l = lexer->language;
    Tokens t = { 0 };
    // Plain text has no colours at all, numbers included.
    if (l == &PLAIN) { emit(&t, 0, n, CODE_PLAIN); *count = t.count; return t.items; }
    size_t i = 0;
    // Carried over from the line before.
    if (lexer->open_comment && l->block_close) {
        size_t end = find(l->block_close, s, n, 0, false);
        if (end > n) { emit(&t, 0, n, CODE_COMMENT); *count = t.count; return t.items; }
        emit(&t, 0, end, CODE_COMMENT); i = end; lexer->open_comment = false;
    } else if (lexer->open_string) {
        size_t end = find(lexer->open_string, s, n, 0, true);
        if (end > n) { emit(&t, 0, n, CODE_STRING); *count = t.count; return t.items; }
        emit(&t, 0, end, CODE_STRING); i = end; lexer->open_string = NULL;
    }
    size_t plain = i;
    while (i < n) {
        char c = s[i];
        // A block comment first, so Lua's --[[ is not taken for its line comment.
        if (l->block_open && matches(l->block_open, s, n, i)) {
            emit(&t, plain, i, CODE_PLAIN);
            size_t end = find(l->block_close, s, n, i + strlen(l->block_open), false);
            if (end > n) { emit(&t, i, n, CODE_COMMENT); lexer->open_comment = true; *count = t.count; return t.items; }
            emit(&t, i, end, CODE_COMMENT); i = plain = end;
            continue;
        }
        // A comment to the line's end; "#" only where it does not end a word or follow a "$" (shell's `$#`).
        const char *line_comment = NULL;
        for (int k = 0; !line_comment && l->line_comments[k]; k++) if (matches(l->line_comments[k], s, n, i)) line_comment = l->line_comments[k];
        if (line_comment && (!str_eq(line_comment, "#") || i == 0 || (!is_letter(s[i - 1]) && s[i - 1] != '$'))) {
            emit(&t, plain, i, CODE_PLAIN); emit(&t, i, n, CODE_COMMENT);
            *count = t.count; return t.items;
        }
        const char *delim = NULL;
        for (int k = 0; !delim && l->multiline[k]; k++) if (matches(l->multiline[k], s, n, i)) delim = l->multiline[k];
        if (delim) {
            emit(&t, plain, i, CODE_PLAIN);
            size_t end = find(delim, s, n, i + strlen(delim), true);
            if (end > n) { emit(&t, i, n, CODE_STRING); lexer->open_string = delim; *count = t.count; return t.items; }
            emit(&t, i, end, CODE_STRING); i = plain = end;
            continue;
        }
        if (c && strchr(l->quotes, c)) {
            emit(&t, plain, i, CODE_PLAIN);
            char quote[2] = { c, 0 };
            size_t end = find(quote, s, n, i + 1, true);
            if (end > n) end = n;
            emit(&t, i, end, CODE_STRING); i = plain = end;
            continue;
        }
        if (l->dollar && c == '$' && i + 1 < n && (is_letter(s[i + 1]) || s[i + 1] == '_' || s[i + 1] == '{')) {
            emit(&t, plain, i, CODE_PLAIN);
            size_t j = i + 1;
            if (s[j] == '{') { while (j < n && s[j] != '}') j++; j = j < n ? j + 1 : n; }
            else while (j < n && is_word_char(s[j])) j++;
            emit(&t, i, j, CODE_VARIABLE); i = plain = j;
            continue;
        }
        if (is_digit(c) && (i == 0 || !is_word_char(s[i - 1]))) {
            emit(&t, plain, i, CODE_PLAIN);
            size_t j = i + 1;
            while (j < n && (is_hex(s[j]) || s[j] == '.' || s[j] == '_' || s[j] == 'x' || s[j] == 'X')) j++;
            emit(&t, i, j, CODE_NUMBER); i = plain = j;
            continue;
        }
        if (is_word_start(c) && (i == 0 || !is_word_char(s[i - 1]))) {
            size_t j = i + 1;
            while (j < n && is_word_char(s[j])) j++;
            // A keyword may end in ? (Ruby's defined?).
            if (j < n && s[j] == '?' && is_keyword(l, s + i, j + 1 - i)) j++;
            if (is_keyword(l, s + i, j - i)) { emit(&t, plain, i, CODE_PLAIN); emit(&t, i, j, CODE_KEYWORD); plain = j; }
            else if (l->types) {
                size_t first = i;
                while (first < j && (s[first] == '@' || s[first] == '#')) first++;
                bool lower = false;
                for (size_t k = first; k < j; k++) if (s[k] >= 'a' && s[k] <= 'z') { lower = true; break; }
                if (first < j && s[first] >= 'A' && s[first] <= 'Z' && lower) { emit(&t, plain, i, CODE_PLAIN); emit(&t, i, j, CODE_TYPE); plain = j; }
            }
            i = j;
            continue;
        }
        i++;
    }
    emit(&t, plain, n, CODE_PLAIN);
    *count = t.count;
    return t.items;
}
