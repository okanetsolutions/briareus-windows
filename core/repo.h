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

#endif
