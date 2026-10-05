// Agent replies are Markdown. Blocks are split out here; the app lays their inline spans out itself.
#ifndef BRIAREUS_MARKDOWN_H
#define BRIAREUS_MARKDOWN_H
#include <stddef.h>

typedef enum { MD_PARAGRAPH, MD_HEADING, MD_BULLET, MD_QUOTE, MD_CODE, MD_RULE, MD_TABLE } MdBlockKind;

typedef struct {
    MdBlockKind kind;
    int level;        // heading 1...6
    int indent;       // bullet nesting
    char *marker;     // bullet: "•" or "2."
    char *text;       // paragraph, heading, bullet, quote and code text
    char *language;   // code fence info string, or NULL
    char **cells;     // table: rows * cols cell texts, header row first
    size_t rows, cols;
    char *aligns;     // table: one of l, c, r per column
    int task;         // bullet: 0 plain, 1 unchecked task, 2 checked task
} MdBlock;

MdBlock *md_parse(const char *source, size_t *count);
void md_free(MdBlock *blocks, size_t count);
/// A table block written back as Markdown, pipes in cells escaped, as a new string.
char *md_table_source(const MdBlock *table);

enum { SPAN_BOLD = 1, SPAN_ITALIC = 2, SPAN_CODE = 4, SPAN_LINK = 8, SPAN_STRIKE = 16 };
typedef struct { unsigned flags; char *text; char *url; } MdSpan;

/// Inline syntax: `code`, **bold**, *italic*, ~~strike~~, [text](url), ![alt](url), <url>, bare URLs and HTML entities. Newlines stay in the text.
MdSpan *md_inline(const char *text, size_t *count);
void md_spans_free(MdSpan *spans, size_t count);
/// The text with inline markers removed, as a new string.
char *md_plain(const char *text);

#endif
