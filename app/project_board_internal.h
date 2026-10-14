#ifndef BRIAREUS_PROJECT_BOARD_INTERNAL_H
#define BRIAREUS_PROJECT_BOARD_INTERNAL_H
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// A project's Board tab, after Issues: the GitHub Projects board named in the project's settings, filtered and grouped
// the way its view is on GitHub (`project_board`). Columns run side by side, each with its count and its number fields
// totalled (Story Points). The columns reach the view's bottom and each scrolls on its own; a wide board scrolls sideways
// with the bar at the bottom, or Shift and the wheel. An assignee picker narrows the cards, as GitHub's filter bar does.
// Only this project's cards are shown, so a board shared by several repositories reads as this one's. A card opens its
// issue or pull request in a side panel over the board, as GitHub's board does. An issue card lists the pull requests
// that close it, from the host's `pulls` read; this project's open the same way, others on GitHub. A card dragged to
// another column moves there at once and on GitHub through `project_board_move`, as dragging it on GitHub's board
// does; a refusal puts the board back as GitHub has it, and a read that lags behind the move keeps it where it went.

// The actions, from the host's `action_base` up.
enum { A_CARD, A_OPEN_GITHUB, A_FILTER_ASSIGNEE, A_CLEAR_FILTER, A_CARD_PULL };

enum { COL_W = 300, COL_MAX_W = 380, COL_GAP = 12, COL_MIN_H = 240 };

// How long a card GitHub moved is put in its new column by hand when a board read still shows it in the old one.
enum { SETTLE_SECONDS = 120 };

/// A card GitHub moved on a repository's board, the column it came from and the one it went to (NULL for the
/// "No <field>" column).
// `from` holds every column the card was carried from while it settles (NULL for the "No <field>" column): a read
// lagging behind more than one quick move shows it in any of them.
typedef struct { char *repo, *id, **from, *column; size_t from_count; time_t until; } Settling;

extern Settling *project_board_settling;
extern size_t project_board_settling_count;
struct BoardTab {
    Project project;
    Screen *host;
    int base;
    ProjectBoard board;
    bool has_board;        // `board` holds an answer, the saved one or the server's
    bool loaded;           // the server answered since the tab was first opened
    char *error;           // the request's own failure; GitHub's refusal is `board.error`
    char *assignee;        // the picked assignee (a login or PROJECT_NO_ASSIGNEE); NULL or empty for everyone
    // The host's `pulls` read, borrowed: which pull requests close each issue card.
    const IssueSummary *issues; size_t issue_count;
    const PullSummary *pulls; size_t pull_count;
    Request *req;
    // Dragging a card: where the columns are, the one under the carried card, and the move the server is making.
    int columns_top, column_w, column_step;
    int drop_column;       // -1 for none
    char *move_error;      // why the last move failed
    Request *move_req;
    // The card the last move carried and where it was, until a board read shows it as GitHub has it.
    char *moved_id; size_t moved_from, moved_at;
    char *moved_off;       // the column it was carried from; NULL for the "No <field>" column
    char *moved_to;        // the column it was carried to; NULL for the "No <field>" column
};

bool board_tab_offered(const Project *project);
void project_board_relayout(BoardTab *p);
void project_board_settled(size_t i);
void project_board_load(BoardTab *p, bool fresh);
void board_tab_set_pulls(BoardTab *p, const IssueSummary *issues, size_t issue_count, const PullSummary *pulls, size_t pull_count);
void board_tab_open(BoardTab *p);
void board_tab_refresh(BoardTab *p);
const ProjectCard *project_board_card_at(const BoardTab *p, intptr_t arg);
BoardLink *project_board_card_pulls(const BoardTab *p, const ProjectCard *card, size_t *count);
void project_board_card_pulls_free(BoardLink *links, size_t count);
void board_tab_layout(BoardTab *p, Doc *doc, int w);
void board_tab_header(BoardTab *p, HeaderInfo *info);
bool board_tab_action(BoardTab *p, int action, intptr_t arg, POINT pt);
void project_board_undo_move(BoardTab *p);
void board_tab_drag(BoardTab *p, int action, intptr_t arg, POINT pt, DragPhase phase);
BoardTab *board_tab_new(const Project *project, Screen *host, int action_base);
void board_tab_free(BoardTab *p);

#endif
