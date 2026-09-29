// One line of a unified diff patch, numbered from its hunk header.
#ifndef BRIAREUS_DIFF_H
#define BRIAREUS_DIFF_H
#include <stddef.h>

typedef enum { DIFF_HUNK, DIFF_ADDED, DIFF_REMOVED, DIFF_CONTEXT, DIFF_NOTE } DiffKind;

typedef struct {
    int id;
    DiffKind kind;
    char *text;
    int old_line, new_line;   // 0 when the side has no number
} DiffLine;

/// Parses a patch into lines; `*count` receives how many.
DiffLine *diff_parse(const char *patch, size_t *count);
void diff_free(DiffLine *lines, size_t count);

#endif
