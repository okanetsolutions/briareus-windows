// A project's repository as the Files tab browses it: the tree in the order it is listed, Go to File, one file, its lines.
#include "repo.h"
#include "str.h"
#include <stdlib.h>
#include <string.h>

// MARK: - Names

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static char lower(char c) { return is_upper(c) ? (char)(c - 'A' + 'a') : c; }

int repo_name_compare(const char *a, size_t alen, const char *b, size_t blen) {
    size_t i = 0, j = 0;
    while (i < alen && j < blen) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            // Runs of digits by their value: leading zeros dropped, then the longer run is the larger number.
            while (i < alen && a[i] == '0' && i + 1 < alen && is_digit(a[i + 1])) i++;
            while (j < blen && b[j] == '0' && j + 1 < blen && is_digit(b[j + 1])) j++;
            size_t ai = i, bj = j;
            while (ai < alen && is_digit(a[ai])) ai++;
            while (bj < blen && is_digit(b[bj])) bj++;
            if (ai - i != bj - j) return ai - i < bj - j ? -1 : 1;
            int c = memcmp(a + i, b + j, ai - i);
            if (c) return c < 0 ? -1 : 1;
            i = ai; j = bj;
            continue;
        }
        char ca = lower(a[i]), cb = lower(b[j]);
        if (ca != cb) return (unsigned char)ca < (unsigned char)cb ? -1 : 1;
        i++; j++;
    }
    if (alen - i != blen - j) return alen - i < blen - j ? -1 : 1;
    return 0;
}

// MARK: - Tree

static int path_compare(const void *x, const void *y) { return strcmp(((const RepoEntry *)x)->path, ((const RepoEntry *)y)->path); }

/// Two entries in the order the tab lists them: a folder before what is in it, and at each level folders before files,
/// then by name as people sort them, then byte by byte so the order is total.
static int tree_compare(const RepoEntry *x, const RepoEntry *y) {
    const char *a = x->path, *b = y->path;
    for (;;) {
        const char *as = strchr(a, '/'), *bs = strchr(b, '/');
        size_t al = as ? (size_t)(as - a) : strlen(a), bl = bs ? (size_t)(bs - b) : strlen(b);
        if (al == bl && memcmp(a, b, al) == 0) {
            if (!as && !bs) return 0;
            if (!as) return -1;   // `x` holds `y`
            if (!bs) return 1;
            a = as + 1; b = bs + 1;
            continue;
        }
        bool fa = as || x->folder, fb = bs || y->folder;
        if (fa != fb) return fa ? -1 : 1;
        int c = repo_name_compare(a, al, b, bl);
        if (c) return c;
        c = memcmp(a, b, al < bl ? al : bl);
        if (c) return c < 0 ? -1 : 1;
        return al < bl ? -1 : 1;
    }
}
typedef struct { RepoEntry entry; size_t rank; } Ranked;
static int ranked_compare(const void *x, const void *y) { return tree_compare(&((const Ranked *)x)->entry, &((const Ranked *)y)->entry); }

static int find_sorted(const RepoEntry *entries, size_t count, const char *path) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(entries[mid].path, path);
        if (!c) return (int)mid;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}
/// Sorts by path and drops the repeats, keeping the first of each.
static size_t sort_unique(RepoEntry *entries, size_t count) {
    if (count < 2) return count;
    qsort(entries, count, sizeof *entries, path_compare);
    size_t kept = 1;
    for (size_t i = 1; i < count; i++) {
        if (str_eq(entries[i].path, entries[kept - 1].path)) { free(entries[i].path); continue; }
        entries[kept++] = entries[i];
    }
    return kept;
}

bool repo_tree_parse(const Json *value, RepoTree *out) {
    memset(out, 0, sizeof *out);
    const Json *list = json_get(value, "entries");
    if (!json_is_object(value) || !json_is_array(list)) return false;
    size_t n = json_count(list), count = 0, cap = n ? n : 1;
    RepoEntry *entries = xcalloc(cap, sizeof *entries);
    for (size_t i = 0; i < n; i++) {
        const Json *e = json_at(list, i);
        const char *path = json_str_nonempty(json_get(e, "path"));
        if (!path) continue;
        RepoEntry *r = &entries[count++];
        r->path = xstrdup(path);
        r->folder = str_eq(json_str(json_get(e, "type")), "tree");
        double size;
        r->size = !r->folder && json_num(json_get(e, "size"), &size) && size >= 0 ? (long long)size : -1;
    }
    count = sort_unique(entries, count);
    // A folder the list skipped (a truncated tree) is still drawn above the files in it: each round adds the parents
    // missing one level up, until none is.
    for (;;) {
        size_t added = 0;
        for (size_t i = 0, before = count; i < before; i++) {
            const char *slash = strrchr(entries[i].path, '/');
            if (!slash) continue;
            char *parent = xstrndup(entries[i].path, (size_t)(slash - entries[i].path));
            if (find_sorted(entries, before, parent) >= 0) { free(parent); continue; }
            if (count == cap) { cap *= 2; entries = xrealloc(entries, cap * sizeof *entries); }
            memset(&entries[count], 0, sizeof *entries);
            entries[count].path = parent; entries[count].folder = true; entries[count].size = -1;
            count++; added++;
        }
        if (!added) break;
        count = sort_unique(entries, count);
    }
    // Listed in the tab's order, remembering each entry's place by path for repo_tree_find.
    Ranked *ranked = xcalloc(count ? count : 1, sizeof *ranked);
    for (size_t i = 0; i < count; i++) { ranked[i].entry = entries[i]; ranked[i].rank = i; }
    qsort(ranked, count, sizeof *ranked, ranked_compare);
    out->by_path = xcalloc(count ? count : 1, sizeof *out->by_path);
    for (size_t i = 0; i < count; i++) {
        RepoEntry *r = &entries[i];
        *r = ranked[i].entry;
        out->by_path[ranked[i].rank] = i;
        const char *slash = strrchr(r->path, '/');
        r->name = slash ? slash + 1 : r->path;
        r->depth = 0;
        for (const char *p = r->path; *p; p++) if (*p == '/') r->depth++;
    }
    free(ranked);
    out->entries = entries; out->count = count;
    out->ref = xstrdup(json_str_or(json_get(value, "ref"), ""));
    out->sha = json_dup_str(json_get(value, "sha"));
    out->truncated = json_bool_is(json_get(value, "truncated"), true);
    return true;
}

void repo_tree_free(RepoTree *tree) {
    for (size_t i = 0; i < tree->count; i++) free(tree->entries[i].path);
    free(tree->entries); free(tree->by_path); free(tree->ref); free(tree->sha);
    memset(tree, 0, sizeof *tree);
}

int repo_tree_find(const RepoTree *tree, const char *path) {
    if (!path) return -1;
    size_t lo = 0, hi = tree->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        size_t index = tree->by_path[mid];
        int c = strcmp(tree->entries[index].path, path);
        if (!c) return (int)index;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return -1;
}

size_t repo_tree_skip(const RepoTree *tree, size_t index) {
    if (index >= tree->count) return tree->count;
    size_t next = index + 1;
    if (tree->entries[index].folder) while (next < tree->count && tree->entries[next].depth > tree->entries[index].depth) next++;
    return next;
}

void repo_tree_reveal(RepoTree *tree, const char *path) {
    if (!path) return;
    for (const char *slash = strchr(path, '/'); slash; slash = strchr(slash + 1, '/')) {
        char *folder = xstrndup(path, (size_t)(slash - path));
        int index = repo_tree_find(tree, folder);
        if (index >= 0 && tree->entries[index].folder) tree->entries[index].open = true;
        free(folder);
    }
}

size_t repo_tree_file_count(const RepoTree *tree) {
    size_t n = 0;
    for (size_t i = 0; i < tree->count; i++) if (!tree->entries[i].folder) n++;
    return n;
}

// MARK: - Go to file

/// A word starts at the path's start, after / _ - . or a space, and at a capital after a small letter.
static bool word_start(const char *s, size_t i) {
    if (i == 0) return true;
    char prev = s[i - 1];
    return prev == '/' || prev == '_' || prev == '-' || prev == '.' || prev == ' ' || (is_upper(s[i]) && is_lower(prev));
}
/// Greedy from `start`. With `starts`, each character goes at a word's start or right after the last one matched when
/// there is one, else anywhere after the last; without it, at its first place after the last (a plain subsequence).
static bool score_from(const char *query, const char *path, size_t start, bool starts, int *score) {
    size_t n = strlen(path), at = start;
    long long last = -2;
    int total = 0;
    for (const char *q = query; *q; q++) {
        long long found = -1;
        if (starts) for (size_t i = at; i < n; i++) if (lower(path[i]) == *q && (word_start(path, i) || (long long)i == last + 1)) { found = (long long)i; break; }
        if (found < 0) for (size_t i = at; i < n; i++) if (lower(path[i]) == *q) { found = (long long)i; break; }
        if (found < 0) return false;
        if (found == last + 1) total += 8;
        if (word_start(path, (size_t)found)) total += 5;
        long long skipped = found - (long long)at;
        total -= (int)(skipped < 10 ? skipped : 10);
        last = found; at = (size_t)found + 1;
    }
    *score = starts ? total : total - 10;
    return true;
}
/// Word starts first; when that greedy choice leaves the rest of the query unmatched, a plain subsequence, scored lower.
static bool score_in(const char *query, const char *path, size_t start, int *score) {
    return score_from(query, path, start, true, score) || score_from(query, path, start, false, score);
}
bool repo_match_score(const char *query, const char *path, int *score) {
    if (str_empty(query) || !path) return false;
    const char *slash = strrchr(path, '/');
    size_t name = slash ? (size_t)(slash - path) + 1 : 0;
    // Inside the name alone when the query holds no slash and fits there; otherwise across the whole path.
    if (!strchr(query, '/') && score_in(query, path, name, score)) { *score += 1000; return true; }
    return score_in(query, path, 0, score);
}

typedef struct { int score; size_t index, len; const char *path; } Found;
static int found_compare(const void *x, const void *y) {
    const Found *a = x, *b = y;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    if (a->len != b->len) return a->len < b->len ? -1 : 1;
    return strcmp(a->path, b->path);
}
size_t *repo_find_files(const RepoTree *tree, const char *query, size_t limit, size_t *count) {
    *count = 0;
    Str q; str_init(&q);
    for (const char *p = query ? query : ""; *p; p++) if (*p != ' ' && *p != '\t') str_appendc(&q, lower(*p));
    if (!q.len || !limit) { str_free(&q); return NULL; }
    Found *found = NULL; size_t n = 0, cap = 0;
    for (size_t i = 0; i < tree->count; i++) {
        const RepoEntry *e = &tree->entries[i];
        int score;
        if (e->folder || !repo_match_score(q.data, e->path, &score)) continue;
        if (n == cap) { cap = cap ? cap * 2 : 64; found = xrealloc(found, cap * sizeof *found); }
        found[n++] = (Found){ score, i, strlen(e->path), e->path };
    }
    str_free(&q);
    if (!n) { free(found); return NULL; }
    qsort(found, n, sizeof *found, found_compare);
    if (n > limit) n = limit;
    size_t *out = xcalloc(n, sizeof *out);
    for (size_t i = 0; i < n; i++) out[i] = found[i].index;
    free(found);
    *count = n;
    return out;
}

// MARK: - One file

bool repo_file_parse(const Json *value, RepoFile *out) {
    memset(out, 0, sizeof *out);
    const char *path = json_str(json_get(value, "path"));
    if (!json_is_object(value) || !path) return false;
    out->path = xstrdup(path);
    out->ref = xstrdup(json_str_or(json_get(value, "ref"), ""));
    out->url = json_dup_str(json_get(value, "url"));
    double size;
    out->size = json_num(json_get(value, "size"), &size) && size >= 0 ? (long long)size : 0;
    out->content = json_dup_str(json_get(value, "content"));
    out->binary = json_bool_is(json_get(value, "binary"), true);
    out->too_large = json_bool_is(json_get(value, "tooLarge"), true);
    return true;
}
void repo_file_free(RepoFile *file) {
    free(file->path); free(file->ref); free(file->url); free(file->content);
    memset(file, 0, sizeof *file);
}

RepoLine *repo_lines(const char *text, size_t *count) {
    size_t n = 0, cap = 16;
    RepoLine *lines = xcalloc(cap, sizeof *lines);
    const char *s = text ? text : "";
    size_t start = 0, i = 0;
    for (; s[i]; i++) {
        if (s[i] != '\n') continue;
        if (n == cap) { cap *= 2; lines = xrealloc(lines, cap * sizeof *lines); }
        size_t len = i - start;
        if (len && s[i - 1] == '\r') len--;
        lines[n++] = (RepoLine){ start, len };
        start = i + 1;
    }
    if (start < i) {
        if (n == cap) { cap *= 2; lines = xrealloc(lines, cap * sizeof *lines); }
        size_t len = i - start;
        if (s[i - 1] == '\r') len--;
        lines[n++] = (RepoLine){ start, len };
    }
    *count = n;
    return lines;
}
