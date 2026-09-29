#include "diff.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

static bool hunk_start(const char *header, int *old_line, int *new_line) {
    // "@@ -10,3 +10,4 @@ context"
    size_t n; char **parts = str_split(header, ' ', &n);
    bool ok = n >= 3 && parts[1][0] == '-' && parts[2][0] == '+';
    if (ok) {
        char *end;
        long o = strtol(parts[1] + 1, &end, 10);
        ok = end != parts[1] + 1 && (*end == 0 || *end == ',');
        long w = 0;
        if (ok) { w = strtol(parts[2] + 1, &end, 10); ok = end != parts[2] + 1 && (*end == 0 || *end == ','); }
        if (ok) { *old_line = (int)o; *new_line = (int)w; }
    }
    str_array_free(parts, n);
    return ok;
}

DiffLine *diff_parse(const char *patch, size_t *count) {
    *count = 0;
    if (!patch) return NULL;
    // Lines split on any newline; a trailing empty line is the patch's final newline, not content.
    size_t raw_count = 0, cap = 64;
    char **raw = xmalloc(cap * sizeof *raw);
    const char *p = patch;
    for (;;) {
        const char *end = p;
        while (*end && *end != '\n' && *end != '\r') end++;
        if (raw_count == cap) { cap *= 2; raw = xrealloc(raw, cap * sizeof *raw); }
        raw[raw_count++] = xstrndup(p, (size_t)(end - p));
        if (!*end) break;
        if (*end == '\r' && end[1] == '\n') end++;
        p = end + 1;
    }
    if (raw_count && raw[raw_count - 1][0] == 0) { free(raw[--raw_count]); }
    DiffLine *lines = xcalloc(raw_count ? raw_count : 1, sizeof *lines);
    int old_line = 0, new_line = 0;
    for (size_t i = 0; i < raw_count; i++) {
        const char *line = raw[i];
        DiffLine *d = &lines[i];
        d->id = (int)i;
        if (str_has_prefix(line, "@@")) {
            int o, w;
            if (hunk_start(line, &o, &w)) { old_line = o; new_line = w; }
            d->kind = DIFF_HUNK; d->text = xstrdup(line);
        } else if (line[0] == '\\') {
            const char *t = line + 1; while (*t == ' ') t++;
            d->kind = DIFF_NOTE; d->text = xstrdup(t);
        } else if (line[0] == '+') {
            d->kind = DIFF_ADDED; d->text = xstrdup(line + 1); d->new_line = new_line++;
        } else if (line[0] == '-') {
            d->kind = DIFF_REMOVED; d->text = xstrdup(line + 1); d->old_line = old_line++;
        } else {
            d->kind = DIFF_CONTEXT; d->text = xstrdup(line[0] ? line + 1 : line); d->old_line = old_line++; d->new_line = new_line++;
        }
    }
    str_array_free(raw, raw_count);
    *count = raw_count;
    return lines;
}

void diff_free(DiffLine *lines, size_t count) {
    if (!lines) return;
    for (size_t i = 0; i < count; i++) free(lines[i].text);
    free(lines);
}
