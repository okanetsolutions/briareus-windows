// The Issues tab past what the board lists. A board read from GitHub stops after its first 100 open issues and says so
// (`issuesTruncated`); `issues` serves the rest from the server's mirror a page at a time, in the board's order (most
// recently updated first). The board screen keeps one of these beside the board and lists the board's issues followed
// by the ones read here.
#ifndef BRIAREUS_ISSUE_PAGES_H
#define BRIAREUS_ISSUE_PAGES_H
#include "board.h"
#include "json.h"
#include <stdbool.h>
#include <stddef.h>

/// Rows a page asks for, and how far each page reaches back before where the last one (or the board) ended: the list
/// shifts between reads as issues are opened, updated or closed, and a row pushed across the edge would otherwise be
/// read by neither.
enum { ISSUE_PAGES_SIZE = 50, ISSUE_PAGES_OVERLAP = 10 };

/// The issues read past the board's, where the next page starts, and why the last read failed. A failed page is read
/// again from the same offset, so Load more doubles as Retry. Zeroed is empty; issue_pages_clear frees and empties it.
typedef struct {
    IssueSummary *issues; size_t count;   // owned: each number once, in the server's order, as first seen
    int next_offset; bool answered;       // where the next page starts; not answered until a page does
    bool ended;                           // the server said no page follows the last one read
    bool mirror_cut;                      // the last page said the server's mirror stopped short of every open issue
    char *error;                          // why the last read failed; owned
} IssuePages;

void issue_pages_clear(IssuePages *pages);

/// Whether the board's answer lists every open issue, so one missing from it is no longer open: not when GitHub refused
/// its issues, nor when it stopped short (the rest are read a page at a time).
bool board_lists_every_issue(const Json *board);
/// The board stopped short of every open issue, and it could read them at all.
bool issue_pages_offered(const Json *board);
/// Load more is offered while the board stopped short, the pages have not ended, and the server has `issues` (`routed`).
bool issue_pages_can_load_more(const IssuePages *pages, const Json *board, bool routed);
/// The board stopped short and nothing reads past it: the server has no `issues` (`routed`), or its pages ended with
/// the mirror itself cut or without a row the board lacks. The list then says it is cut.
bool issue_pages_cut_short(const IssuePages *pages, const Json *board, bool routed);
/// Load more reads a failed page again.
bool issue_pages_retries(const IssuePages *pages);
/// Why the last read failed, while Load more is there to read it again; borrowed, or NULL.
const char *issue_pages_retry_error(const IssuePages *pages, const Json *board, bool routed);
/// The next page's arguments (owned): after the rows the board lists, then wherever the last page said, but never
/// further inside the board's rows (which a newer board may hold more of) than ISSUE_PAGES_OVERLAP reaches back.
Json *issue_pages_query(const IssuePages *pages, const char *repo, const Json *board);
/// A read is under way: the last failure goes, so progress shows rather than an old error.
void issue_pages_reading(IssuePages *pages);
/// Keeps the page's rows no earlier page holds, and where the next starts. Rows the board lists too are kept, so one a
/// newer board pushes off its first 100 stays listed. False, changing nothing, for an answer that is not a page.
bool issue_pages_receive(IssuePages *pages, const Json *page);
/// The page could not be read (`error` is copied); Load more reads it again from the same offset.
void issue_pages_fail(IssuePages *pages, const char *error);
/// An issue the app closed: the board drops it on its next read, but a row read here stays until a refresh unless it
/// goes too.
void issue_pages_drop(IssuePages *pages, int number);
/// The board's issues followed by the ones read here that it does not list, as a new array of shallow copies: free
/// the array only, while `board_issues` and `pages` still hold the rows. A board that no longer stops short lists
/// every open issue itself, so what was read is left out.
IssueSummary *issue_pages_listed(const IssuePages *pages, const Json *board, const IssueSummary *board_issues, size_t board_count, size_t *count);

#endif
