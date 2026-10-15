#include "project_board_internal.h"

static int column_at(const BoardTab *p, POINT pt);
static void move_done(void *owner, Request *req);
static void move_card(BoardTab *p, size_t from, size_t card, size_t to);

// MARK: - Moving cards

/// The column under a content point, or -1 above the columns or between them.
static int column_at(const BoardTab *p, POINT pt) {
    if (pt.y < p->columns_top || pt.x < 0 || p->column_step <= 0) return -1;
    int k = pt.x / p->column_step;
    if (k >= (int)p->board.column_count || pt.x - k * p->column_step >= p->column_w) return -1;
    return k;
}

static void move_done(void *owner, Request *req) {
    BoardTab *p = owner;
    // Moved or not, the board is read again as GitHub has it now: the server has dropped its saved copy.
    if (!req->ok) request_error_into(&p->move_error, req);
    else if (p->moved_id) {
        // A card moved again before it settled keeps the columns it came from before, so a read behind both moves
        // is still corrected.
        char **from = NULL; size_t from_count = 0;
        for (size_t i = 0; i < project_board_settling_count; i++) {
            if (!str_eq(project_board_settling[i].repo, p->project.repo) || !str_eq(project_board_settling[i].id, p->moved_id)) continue;
            from = project_board_settling[i].from; from_count = project_board_settling[i].from_count;
            project_board_settling[i].from = NULL; project_board_settling[i].from_count = 0;
            project_board_settled(i); break;
        }
        from = xrealloc(from, (from_count + 1) * sizeof *from);
        from[from_count++] = p->moved_off; p->moved_off = NULL;
        project_board_settling = xrealloc(project_board_settling, (project_board_settling_count + 1) * sizeof *project_board_settling);
        Settling *m = &project_board_settling[project_board_settling_count++];
        m->repo = xstrdup(p->project.repo);
        m->id = p->moved_id; p->moved_id = NULL;
        m->from = from; m->from_count = from_count;
        m->column = p->moved_to; p->moved_to = NULL;
        m->until = time(NULL) + SETTLE_SECONDS;
    }
    project_board_load(p, !req->ok);
    project_board_relayout(p);
}

/// The card shows in its new column at once, at its end; the board read after the server answers puts it in its place.
static void move_card(BoardTab *p, size_t from, size_t card, size_t to) {
    if (from >= p->board.column_count || to >= p->board.column_count || card >= p->board.columns[from].card_count) return;
    Json *args = json_object();
    json_set_str(args, "repo", p->project.repo);
    json_set_str(args, "itemId", p->board.columns[from].cards[card].id);
    const char *column = p->board.columns[to].id;
    if (column) json_set_str(args, "columnId", column); else json_object_set(args, "columnId", json_null());
    if (!project_board_move(&p->board, from, card, to)) { json_free(args); return; }
    set_string(&p->moved_id, json_str_nonempty(json_get(args, "itemId")));
    set_string(&p->moved_off, p->board.columns[from].id);
    set_string(&p->moved_to, column);
    p->moved_from = from; p->moved_at = card;
    set_string(&p->move_error, NULL);
    // A read on its way would show the card back where it was; the one after the move replaces it.
    request_cancel(&p->req);
    store_call("project_board_move", args, 0, p, move_done, 0, &p->move_req);
    project_board_relayout(p);
}

/// Puts the card a failed move carried back where it was, when no board read has replaced the guess.
void project_board_undo_move(BoardTab *p) {
    if (!p->moved_id || p->moved_from >= p->board.column_count) { set_string(&p->moved_id, NULL); return; }
    for (size_t k = 0; k < p->board.column_count; k++) {
        ProjectColumn *c = &p->board.columns[k];
        for (size_t i = 0; k != p->moved_from && i < c->card_count; i++) {
            if (!str_eq(c->cards[i].id, p->moved_id) || !project_board_move(&p->board, k, i, p->moved_from)) continue;
            ProjectColumn *back = &p->board.columns[p->moved_from];
            size_t at = p->moved_at < back->card_count ? p->moved_at : back->card_count - 1;
            ProjectCard card = back->cards[back->card_count - 1];
            memmove(&back->cards[at + 1], &back->cards[at], (back->card_count - 1 - at) * sizeof *back->cards);
            back->cards[at] = card;
            k = p->board.column_count; break;
        }
    }
    set_string(&p->moved_id, NULL);
}

void board_tab_drag(BoardTab *p, int action, intptr_t arg, POINT pt, DragPhase phase) {
    if (action != p->base + A_CARD) return;
    int from = (int)((size_t)arg >> 16), to = phase == DRAG_CANCEL ? -1 : column_at(p, pt);
    if (to == from) to = -1;
    p->drop_column = phase == DRAG_MOVE ? to : -1;
    if (phase == DRAG_DROP && to >= 0) move_card(p, (size_t)from, (size_t)arg & 0xFFFF, (size_t)to);
}
