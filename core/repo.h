// A project's repository as the Files tab browses it: the tree `repo_tree` lists at one branch, in the order the tab
// shows it (folders before files at every level, names sorted the way people sort them), a file finder that ranks paths
// against a typed query as PhpStorm's Go to File does, one file as `repo_file` answers it, and that file's lines and
// colours (`code_*`, a small lexer per family of languages).
#ifndef BRIAREUS_REPO_H
#define BRIAREUS_REPO_H
#include "json.h"
#include <stdbool.h>
#include <stddef.h>

/// A file or folder. `name` points into `path`, past its last slash.
typedef struct {
    char *path; const char *name;
    int depth;            // 0 at the root
    bool folder;
    bool open;            // a folder the tab shows unfolded; the tab's own state, false as read
    long long size;       // a file's size in bytes; -1 for a folder or when the server sent none
} RepoEntry;

/// What `repo_tree` answers. `entries` are in the order the tab lists them: each folder followed by everything in it.
typedef struct {
    char *ref;            // the branch, tag or commit read
    char *sha;            // the commit it pointed at; files are read at it so they match the tree. NULL when not sent
    bool truncated;       // GitHub stopped listing early (past 100,000 entries)
    RepoEntry *entries; size_t count;
    size_t *by_path;      // indices into `entries` sorted by path, for repo_tree_find
} RepoTree;

/// Needs an object with an `entries` array; entries without a path are skipped, and the folders a truncated list
/// skipped are put back above the files in them.
bool repo_tree_parse(const Json *value, RepoTree *out);
void repo_tree_free(RepoTree *tree);
/// The entry with this path, or -1.
int repo_tree_find(const RepoTree *tree, const char *path);
/// The entry after `index` and everything in it: the next row when `index` is a folder that is folded.
size_t repo_tree_skip(const RepoTree *tree, size_t index);
/// Unfolds the folders that hold `path`, so its row shows.
void repo_tree_reveal(RepoTree *tree, const char *path);
/// How many files the tree lists.
size_t repo_tree_file_count(const RepoTree *tree);
/// Two names as people sort them: case ignored, and runs of digits by their value ("file2" before "file10").
int repo_name_compare(const char *a, size_t alen, const char *b, size_t blen);

/// The files a query finds, best first, at most `limit` of them, as indices into `tree->entries`: the query's
/// characters in order, case and spaces ignored, and a "/" in it matching across folders. A match inside the file's own
/// name beats one spread over its folders, a run of characters beats scattered ones, and a match at a word's start
/// beats one inside it. An empty query finds nothing.
size_t *repo_find_files(const RepoTree *tree, const char *query, size_t limit, size_t *count);
/// A path's score for a query already lowered and stripped of spaces; false when the query is not in it in order.
bool repo_match_score(const char *query, const char *path, int *score);

/// What `repo_file` answers for one file.
typedef struct {
    char *path, *ref, *url;
    long long size;
    char *content;        // the text; NULL for a file shown only by its size
    bool binary;          // it does not read as UTF-8 text
    bool too_large;       // it is over 1 MB, and only its size is sent
} RepoFile;
/// Needs an object with a string `path`.
bool repo_file_parse(const Json *value, RepoFile *out);
void repo_file_free(RepoFile *file);

/// One line of a text, as offsets into it: no line break, and no "\r" before one.
typedef struct { size_t start, len; } RepoLine;
/// The lines of a text; one that ends in a line break has no empty line after it. Never NULL.
RepoLine *repo_lines(const char *text, size_t *count);

// MARK: - Colours

typedef enum { CODE_PLAIN, CODE_KEYWORD, CODE_STRING, CODE_COMMENT, CODE_NUMBER, CODE_TYPE, CODE_VARIABLE } CodeKind;
/// A run of one line, as byte offsets into the line.
typedef struct { size_t start, len; CodeKind kind; } CodeToken;
typedef struct CodeLanguage CodeLanguage;
/// The language a file is read in, from its name; plain text when none fits. Never NULL.
const CodeLanguage *code_language_of(const char *path);
/// "PHP", "Text", ...
const char *code_language_name(const CodeLanguage *language);

/// Reads a file line after line, carrying a block comment or a multi-line string from one line into the next.
typedef struct { const CodeLanguage *language; bool open_comment; const char *open_string; } CodeLexer;
void code_lexer_init(CodeLexer *lexer, const CodeLanguage *language);
/// A line's runs in order, plain ones included and neighbours of one kind merged, so together they cover the line.
/// NULL with `*count` 0 for an empty line.
CodeToken *code_lexer_line(CodeLexer *lexer, const char *line, size_t len, size_t *count);

// MARK: - Local index

/// The most files an index holds; GitHub's rate limit makes more a long wait. And the most bytes of source: the cache
/// reads back no file over 256 MB, so an index past that would be read anew on every launch.
enum { REPO_INDEX_MAX_FILES = 5000, REPO_INDEX_MAX_BYTES = 192 * 1024 * 1024 };
/// Whether a file of the tree is worth indexing: source or text in a language the colours know, at most 1 MB, and not
/// under a folder of dependencies or build output (vendor/, node_modules/, dist/, ...), minified, a source map or a lock file.
bool repo_indexable(const char *path, long long size);

typedef enum { SYMBOL_CLASS, SYMBOL_FUNCTION, SYMBOL_CONSTANT } SymbolKind;
/// A declaration found in a file: a class (interface, trait, enum, struct, type, ...), a function or method, or a constant.
typedef struct {
    char *name;
    char *container;      // the class a method or constant is declared in; NULL at the top level
    SymbolKind kind;
    size_t file;          // the index's file it is in
    int line;             // from 1
} RepoSymbol;
/// The declarations of one file, found line by line outside comments and strings; `file` is 0 in each.
RepoSymbol *repo_symbols_of(const char *path, const char *content, size_t len, size_t *count);
void repo_symbols_free(RepoSymbol *symbols, size_t count);

typedef struct { char *path; long long size; char *content; size_t len; } RepoIndexFile;
/// A repository's indexable files as text on this PC, for Go to Class, Go to Symbol and Find in Files.
typedef struct {
    char *ref;            // the branch it follows; NULL for the default one
    char *sha;            // the commit every file is at; NULL while it is being built
    char *partial;        // the commit a build not yet finished reads its files at
    // Files keep their place as others are added, so the symbols' `file` stays right while an index is filled;
    // removing one moves those after it, and repo_index_symbols is then due.
    RepoIndexFile *files; size_t count, cap;
    size_t bytes;         // the files' contents, together
    size_t *order;        // indices into `files` by path
    RepoSymbol *symbols; size_t symbol_count;  // from repo_index_symbols
} RepoIndex;
void repo_index_init(RepoIndex *index);
void repo_index_free(RepoIndex *index);
int repo_index_find(const RepoIndex *index, const char *path);
/// Adds a file, or replaces the one at its path; the content is copied.
void repo_index_put(RepoIndex *index, const char *path, long long size, const char *content, size_t len);
bool repo_index_remove(RepoIndex *index, const char *path);
/// Every file's declarations, worked out again.
void repo_index_symbols(RepoIndex *index);
/// The index as bytes for the disk, and back; parsing fails on anything it did not write.
char *repo_index_serialize(const RepoIndex *index, size_t *len);
bool repo_index_parse(const char *data, size_t len, RepoIndex *out);
/// Brings the index in line with a tree before it is read at the tree's commit: drops the files the tree no longer has
/// or that are not indexable, and those `changed` names (they are read again), then lists the indexable files of the
/// tree the index lacks, up to REPO_INDEX_MAX_FILES and REPO_INDEX_MAX_BYTES (as the tree gives sizes) in all. Returns how many paths `*fetch` holds.
size_t repo_index_reconcile(RepoIndex *index, const RepoTree *tree, char *const *changed, size_t changed_count, char ***fetch);

/// What `GET /commits/{sha}` says for walking back to the index's commit: the paths it changed (old names of renamed
/// files too), its first parent (NULL for none), and whether GitHub cut the list short.
bool repo_commit_changes(const Json *value, char ***paths, size_t *count, char **parent, bool *truncated);

/// The declarations a query finds, best first, as indices into `index->symbols`: classes alone when `classes` is set.
/// Matched against the name as Go to File matches a file's name.
size_t *repo_index_find_symbols(const RepoIndex *index, const char *query, bool classes, size_t limit, size_t *count);
/// A line holding the text searched for; `start` is where the line begins in the file's content.
typedef struct { size_t file; int line; size_t column, start; } RepoTextHit;
/// The lines holding `query`, case ignored, file by file in path order, at most `limit`; `*total` counts them all.
RepoTextHit *repo_index_find_text(const RepoIndex *index, const char *query, size_t limit, size_t *count, size_t *total);

#endif
