#include "markdown.h"
#include "str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

typedef struct { MdBlock *items; size_t count, cap; } Blocks;

static MdBlock *push(Blocks *b) {
    if (b->count == b->cap) { b->cap = b->cap ? b->cap * 2 : 16; b->items = xrealloc(b->items, b->cap * sizeof *b->items); }
    MdBlock *block = &b->items[b->count++];
    memset(block, 0, sizeof *block);
    return block;
}

static char *join_lines(char **lines, size_t n) {
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) { if (i) str_appendc(&s, '\n'); str_appendz(&s, lines[i]); }
    return str_detach(&s);
}

typedef struct { char **lines; size_t count, cap; } Lines;
static void lines_push(Lines *l, const char *line) {
    if (l->count == l->cap) { l->cap = l->cap ? l->cap * 2 : 8; l->lines = xrealloc(l->lines, l->cap * sizeof *l->lines); }
    l->lines[l->count++] = xstrdup(line);
}
static void lines_clear(Lines *l) { for (size_t i = 0; i < l->count; i++) free(l->lines[i]); l->count = 0; }

static void flush(Blocks *b, Lines *paragraph, Lines *quote) {
    if (paragraph->count) { MdBlock *k = push(b); k->kind = MD_PARAGRAPH; k->text = join_lines(paragraph->lines, paragraph->count); lines_clear(paragraph); }
    if (quote->count) { MdBlock *k = push(b); k->kind = MD_QUOTE; k->text = join_lines(quote->lines, quote->count); lines_clear(quote); }
}

static bool heading(const char *trimmed, int *level, const char **text) {
    int hashes = 0;
    while (trimmed[hashes] == '#') hashes++;
    if (hashes < 1 || hashes > 6 || trimmed[hashes] != ' ') return false;
    *level = hashes;
    const char *t = trimmed + hashes + 1;
    while (*t == ' ') t++;
    *text = t;
    return true;
}

static bool bullet(const char *line, int *indent, char **marker, char **text) {
    size_t spaces = 0;
    while (line[spaces] == ' ' || line[spaces] == '\t') spaces++;
    const char *rest = line + spaces;
    if ((rest[0] == '-' || rest[0] == '*' || rest[0] == '+') && rest[1] == ' ') {
        *indent = (int)(spaces / 2); *marker = xstrdup("\xE2\x80\xA2"); *text = xstrdup(rest + 2);
        return true;
    }
    size_t digits = 0;
    while (isdigit((unsigned char)rest[digits])) digits++;
    if (digits >= 1 && digits <= 3 && (rest[digits] == '.' || rest[digits] == ')') && rest[digits + 1] == ' ') {
        *indent = (int)(spaces / 2);
        *marker = xstrfmt("%.*s.", (int)digits, rest);
        *text = xstrdup(rest + digits + 2);
        return true;
    }
    return false;
}

/// A pipe-delimited row split into trimmed cells; outer pipes are optional.
static char **table_cells(const char *line, size_t *count) {
    const char *p = line; while (*p == ' ' || *p == '\t') p++;
    if (*p == '|') p++;
    char *copy = xstrdup(p);
    size_t n = strlen(copy); while (n && (copy[n - 1] == ' ' || copy[n - 1] == '\t')) copy[--n] = 0;
    if (n && copy[n - 1] == '|' && (n < 2 || copy[n - 2] != '\\')) copy[--n] = 0;
    char **cells = NULL; size_t m = 0;
    const char *start = copy;
    for (const char *q = copy;; q++) {
        if (*q == '\\' && q[1] == '|') { q++; continue; }
        if (*q == '|' || !*q) {
            char *raw = xstrndup(start, (size_t)(q - start));
            char *unescaped = str_replace(raw, "\\|", "|"); free(raw);
            cells = xrealloc(cells, (m + 1) * sizeof *cells); cells[m++] = str_trim(unescaped); free(unescaped);
            if (!*q) break;
            start = q + 1;
        }
    }
    free(copy);
    *count = m;
    return cells;
}
/// "|---|:--:|--:|" says a table starts; fills the alignment of each column.
static bool table_delimiter(const char *line, char **aligns, size_t *cols) {
    size_t n; char **cells = table_cells(line, &n);
    bool ok = n > 0;
    char *al = xmalloc(n + 1);
    for (size_t i = 0; i < n && ok; i++) {
        const char *c = cells[i]; size_t len = strlen(c);
        bool left = len && c[0] == ':', right = len && c[len - 1] == ':';
        size_t dashes = 0;
        for (size_t k = 0; k < len; k++) if (c[k] == '-') dashes++; else if (c[k] != ':' && c[k] != ' ') ok = false;
        if (dashes < 1) ok = false;
        al[i] = left && right ? 'c' : right ? 'r' : 'l';
    }
    al[n] = 0;
    str_array_free(cells, n);
    if (!ok) { free(al); return false; }
    *aligns = al; *cols = n;
    return true;
}
static bool is_table_row(const char *trimmed) { return strchr(trimmed, '|') != NULL; }

static bool is_rule(const char *trimmed) {
    size_t n = strlen(trimmed);
    if (n < 3) return false;
    char first = trimmed[0];
    if (first != '-' && first != '*' && first != '_') return false;
    size_t marks = 0;
    for (size_t i = 0; i < n; i++) {
        if (trimmed[i] == first) marks++;
        else if (trimmed[i] != ' ') return false;
    }
    return marks >= 3;
}

MdBlock *md_parse(const char *source, size_t *count) {
    Blocks b = { 0 };
    Lines paragraph = { 0 }, quote = { 0 }, fence_lines = { 0 };
    bool in_fence = false; char fence_char = 0; size_t fence_len = 0; char *fence_language = NULL;
    char *normalized = str_replace(source ? source : "", "\r\n", "\n");
    size_t n; char **lines = str_split(normalized, '\n', &n);
    for (size_t i = 0; i < n; i++) {
        const char *line = lines[i];
        char *trimmed = str_trim(line);
        if (in_fence) {
            size_t k = 0; while (trimmed[k] == fence_char) k++;
            if (k >= fence_len && trimmed[k] == 0) {
                MdBlock *code = push(&b); code->kind = MD_CODE; code->language = fence_language; fence_language = NULL;
                code->text = join_lines(fence_lines.lines, fence_lines.count); lines_clear(&fence_lines); in_fence = false;
            } else lines_push(&fence_lines, line);
            free(trimmed);
            continue;
        }
        if (str_has_prefix(trimmed, "```") || str_has_prefix(trimmed, "~~~")) {
            flush(&b, &paragraph, &quote);
            fence_char = trimmed[0]; fence_len = 0;
            while (trimmed[fence_len] == fence_char) fence_len++;
            char *language = str_trim(trimmed + fence_len);
            fence_language = *language ? language : (free(language), NULL);
            in_fence = true;
            free(trimmed);
            continue;
        }
        if (!*trimmed) { flush(&b, &paragraph, &quote); free(trimmed); continue; }
        if (trimmed[0] == '>') {
            if (paragraph.count) flush(&b, &paragraph, &quote);
            char *q = str_trim(trimmed + 1); lines_push(&quote, q); free(q); free(trimmed);
            continue;
        }
        if (quote.count) flush(&b, &paragraph, &quote);
        // A table: a header row, a delimiter row, then rows until a blank line.
        if (is_table_row(trimmed) && i + 1 < n) {
            char *next = str_trim(lines[i + 1]); char *aligns; size_t cols;
            bool starts = table_delimiter(next, &aligns, &cols);
            free(next);
            if (starts) {
                size_t hn; char **header = table_cells(line, &hn);
                if (hn == cols) {
                    flush(&b, &paragraph, &quote);
                    MdBlock *t = push(&b); t->kind = MD_TABLE; t->aligns = aligns; t->cols = cols;
                    t->cells = xmalloc(cols * sizeof *t->cells);
                    for (size_t c = 0; c < cols; c++) t->cells[c] = header[c];
                    free(header); t->rows = 1;
                    size_t k = i + 2;
                    for (; k < n; k++) {
                        char *rt = str_trim(lines[k]);
                        bool row = *rt && is_table_row(rt);
                        free(rt);
                        if (!row) break;
                        size_t rn; char **cells = table_cells(lines[k], &rn);
                        t->cells = xrealloc(t->cells, (t->rows + 1) * cols * sizeof *t->cells);
                        for (size_t c = 0; c < cols; c++) t->cells[t->rows * cols + c] = c < rn ? cells[c] : xstrdup("");
                        for (size_t c = cols; c < rn; c++) free(cells[c]);
                        free(cells); t->rows++;
                    }
                    i = k - 1;
                    free(trimmed);
                    continue;
                }
                str_array_free(header, hn); free(aligns);
            }
        }
        int level; const char *heading_text;
        if (heading(trimmed, &level, &heading_text)) {
            flush(&b, &paragraph, &quote);
            MdBlock *k = push(&b); k->kind = MD_HEADING; k->level = level; k->text = str_trim(heading_text);
            free(trimmed); continue;
        }
        if (is_rule(trimmed)) { flush(&b, &paragraph, &quote); push(&b)->kind = MD_RULE; free(trimmed); continue; }
        int indent; char *marker, *text;
        if (bullet(line, &indent, &marker, &text)) {
            flush(&b, &paragraph, &quote);
            MdBlock *k = push(&b); k->kind = MD_BULLET; k->indent = indent; k->marker = marker; k->text = text;
            // A task list item carries its box instead of a bullet.
            if (str_has_prefix(text, "[ ] ") || str_eq(text, "[ ]")) { k->task = 1; memmove(text, text + (text[3] ? 4 : 3), strlen(text) - (text[3] ? 4 : 3) + 1); }
            else if ((str_has_prefix(text, "[x] ") || str_has_prefix(text, "[X] ") || str_eq(text, "[x]") || str_eq(text, "[X]"))) { k->task = 2; memmove(text, text + (text[3] ? 4 : 3), strlen(text) - (text[3] ? 4 : 3) + 1); }
            free(trimmed); continue;
        }
        if (!paragraph.count && b.count && b.items[b.count - 1].kind == MD_BULLET && line[0] == ' ') {
            // Lazy continuation of the previous list item.
            MdBlock *last = &b.items[b.count - 1];
            char *joined = xstrfmt("%s\n%s", last->text, trimmed);
            free(last->text); last->text = joined;
            free(trimmed); continue;
        }
        lines_push(&paragraph, line);
        free(trimmed);
    }
    if (in_fence) {
        MdBlock *code = push(&b); code->kind = MD_CODE; code->language = fence_language; fence_language = NULL;
        code->text = join_lines(fence_lines.lines, fence_lines.count); lines_clear(&fence_lines);
    }
    flush(&b, &paragraph, &quote);
    free(fence_language);
    free(paragraph.lines); free(quote.lines); free(fence_lines.lines);
    str_array_free(lines, n); free(normalized);
    *count = b.count;
    return b.items;
}

void md_free(MdBlock *blocks, size_t count) {
    if (!blocks) return;
    for (size_t i = 0; i < count; i++) {
        free(blocks[i].marker); free(blocks[i].text); free(blocks[i].language); free(blocks[i].aligns);
        if (blocks[i].cells) { for (size_t k = 0; k < blocks[i].rows * blocks[i].cols; k++) free(blocks[i].cells[k]); free(blocks[i].cells); }
    }
    free(blocks);
}

// MARK: - Inline

typedef struct { MdSpan *items; size_t count, cap; } Spans;

static void emit(Spans *s, unsigned flags, const char *text, size_t len, const char *url) {
    if (!len && !url) return;
    if (s->count && s->items[s->count - 1].flags == flags && !url && !s->items[s->count - 1].url) {
        MdSpan *last = &s->items[s->count - 1];
        size_t old = strlen(last->text);
        last->text = xrealloc(last->text, old + len + 1);
        memcpy(last->text + old, text, len); last->text[old + len] = 0;
        return;
    }
    if (s->count == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->items = xrealloc(s->items, s->cap * sizeof *s->items); }
    MdSpan *span = &s->items[s->count++];
    span->flags = flags; span->text = xstrndup(text, len); span->url = xstrdup(url);
}

static bool is_url_start(const char *p) { return str_has_prefix(p, "https://") || str_has_prefix(p, "http://"); }

static void parse_inline(Spans *out, const char *text, size_t len, unsigned flags);

/// Finds the closing `marker` after `p` on the same span; NULL without one. A single marker steps over a doubled span inside it.
static const char *find_close(const char *p, const char *end, const char *marker) {
    size_t m = strlen(marker);
    for (const char *q = p; q + m <= end; q++) {
        if (*q == '\\') { q++; continue; }
        if (m == 1 && q + 2 < end && q[1] == *marker && q[2] != *marker && !isspace((unsigned char)q[2])) {
            const char doubled[3] = { *marker, *marker, 0 };
            const char *inner = find_close(q + 2, end, doubled);
            if (inner) { q = inner + 1; continue; }
        }
        if (memcmp(q, marker, m) == 0 && q > p && !isspace((unsigned char)q[-1])) return q;
    }
    return NULL;
}

static void parse_inline(Spans *out, const char *text, size_t len, unsigned flags) {
    const char *p = text, *end = text + len;
    Str plain; str_init(&plain);
#define FLUSH() do { if (plain.len) { emit(out, flags, plain.data, plain.len, NULL); plain.len = 0; if (plain.data) plain.data[0] = 0; } } while (0)
    while (p < end) {
        char c = *p;
        if (c == '\\' && p + 1 < end && strchr("\\`*_[]()<>#~!|", p[1])) { str_appendc(&plain, p[1]); p += 2; continue; }
        if (c == '&') {
            // The entities GitHub's renderer leaves in agent replies.
            static const struct { const char *name; const char *text; } entities[] = {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&#39;", "'" }, { "&apos;", "'" }, { "&nbsp;", "\xC2\xA0" },
            };
            bool matched = false;
            for (size_t e = 0; e < sizeof entities / sizeof *entities && !matched; e++) {
                size_t n = strlen(entities[e].name);
                if ((size_t)(end - p) >= n && memcmp(p, entities[e].name, n) == 0) { str_appendz(&plain, entities[e].text); p += n; matched = true; }
            }
            if (matched) continue;
        }
        if (c == '~' && p + 2 < end && p[1] == '~' && !isspace((unsigned char)p[2])) {
            const char *close = find_close(p + 2, end, "~~");
            if (close && close > p + 2) {
                FLUSH();
                parse_inline(out, p + 2, (size_t)(close - p - 2), flags | SPAN_STRIKE);
                p = close + 2; continue;
            }
        }
        if (c == '!' && p + 1 < end && p[1] == '[') { p++; c = '['; }
        if (c == '`') {
            size_t ticks = 0; while (p + ticks < end && p[ticks] == '`') ticks++;
            const char *q = p + ticks;
            const char *close = NULL;
            for (; q + ticks <= end; q++) {
                size_t k = 0; while (k < ticks && q[k] == '`') k++;
                if (k == ticks && (q + ticks == end || q[ticks] != '`')) { close = q; break; }
            }
            if (close) {
                FLUSH();
                const char *inner = p + ticks; size_t inner_len = (size_t)(close - inner);
                if (inner_len >= 2 && inner[0] == ' ' && inner[inner_len - 1] == ' ') { inner++; inner_len -= 2; }
                emit(out, flags | SPAN_CODE, inner, inner_len, NULL);
                p = close + ticks; continue;
            }
            str_append(&plain, p, ticks); p += ticks; continue;
        }
        if ((c == '*' || (c == '_' && !(p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_')))) && p + 3 < end && p[1] == c && p[2] == c && !isspace((unsigned char)p[3])) {
            const char *close = find_close(p + 3, end, c == '*' ? "***" : "___");
            if (close && close > p + 3) {
                FLUSH();
                parse_inline(out, p + 3, (size_t)(close - p - 3), flags | SPAN_BOLD | SPAN_ITALIC);
                p = close + 3; continue;
            }
        }
        if ((c == '*' || c == '_') && p + 1 < end) {
            bool doubled = p[1] == c;
            const char *marker = doubled ? (c == '*' ? "**" : "__") : (c == '*' ? "*" : "_");
            size_t m = doubled ? 2 : 1;
            const char *content = p + m;
            // Intraword underscores are literal, as in snake_case names.
            bool word_before = p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '_');
            if (content < end && !isspace((unsigned char)*content) && !(c == '_' && word_before)) {
                const char *close = find_close(content, end, marker);
                if (close && close > content) {
                    FLUSH();
                    parse_inline(out, content, (size_t)(close - content), flags | (doubled ? SPAN_BOLD : SPAN_ITALIC));
                    p = close + m; continue;
                }
            }
            str_appendc(&plain, c); p++; continue;
        }
        if (c == '[') {
            const char *close = NULL; int depth = 0;
            for (const char *q = p; q < end; q++) { if (*q == '[') depth++; else if (*q == ']') { if (--depth == 0) { close = q; break; } } }
            if (close && close + 1 < end && close[1] == '(') {
                // Parentheses inside the URL come in pairs, as in Wikipedia's links.
                const char *paren = NULL; int parens = 0;
                for (const char *q = close + 2; q < end && !paren; q++) { if (*q == '(') parens++; else if (*q == ')' && parens-- == 0) paren = q; }
                if (paren) {
                    char *url = xstrndup(close + 2, (size_t)(paren - close - 2));
                    char *space = strchr(url, ' '); if (space) *space = 0;
                    FLUSH();
                    size_t label_len = (size_t)(close - p - 1);
                    Spans label = { 0 };
                    parse_inline(&label, p + 1, label_len, flags | SPAN_LINK);
                    for (size_t i = 0; i < label.count; i++) {
                        if (out->count == out->cap) { out->cap = out->cap ? out->cap * 2 : 8; out->items = xrealloc(out->items, out->cap * sizeof *out->items); }
                        out->items[out->count] = label.items[i];
                        out->items[out->count].url = xstrdup(url);
                        out->count++;
                    }
                    // An empty label shows the link itself.
                    if (!label.count && *url) emit(out, flags | SPAN_LINK, url, strlen(url), url);
                    free(label.items); free(url);
                    p = paren + 1; continue;
                }
            }
            str_appendc(&plain, c); p++; continue;
        }
        if (c == '<' && is_url_start(p + 1)) {
            const char *close = memchr(p, '>', (size_t)(end - p));
            if (close) {
                FLUSH();
                char *url = xstrndup(p + 1, (size_t)(close - p - 1));
                emit(out, flags | SPAN_LINK, url, strlen(url), url);
                free(url); p = close + 1; continue;
            }
        }
        if (is_url_start(p) && (p == text || !isalnum((unsigned char)p[-1]))) {
            const char *q = p;
            while (q < end && !isspace((unsigned char)*q) && *q != '<' && *q != '>' && *q != '"' && *q != ')') q++;
            while (q > p && strchr(".,;:!?", q[-1])) q--;
            FLUSH();
            char *url = xstrndup(p, (size_t)(q - p));
            emit(out, flags | SPAN_LINK, url, strlen(url), url);
            free(url); p = q; continue;
        }
        str_appendc(&plain, c); p++;
    }
    FLUSH();
#undef FLUSH
    str_free(&plain);
}

MdSpan *md_inline(const char *text, size_t *count) {
    Spans s = { 0 };
    parse_inline(&s, text ? text : "", text ? strlen(text) : 0, 0);
    *count = s.count;
    return s.items;
}

void md_spans_free(MdSpan *spans, size_t count) {
    if (!spans) return;
    for (size_t i = 0; i < count; i++) { free(spans[i].text); free(spans[i].url); }
    free(spans);
}

char *md_plain(const char *text) {
    size_t n; MdSpan *spans = md_inline(text, &n);
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) str_appendz(&s, spans[i].text);
    md_spans_free(spans, n);
    return str_detach(&s);
}
