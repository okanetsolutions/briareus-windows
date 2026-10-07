// A project's Board tab, after Issues: the GitHub Projects board named in the project's settings, filtered and grouped
// the way its view is on GitHub (`project_board`). Columns run side by side, each with its count and its number fields
// totalled (Story Points). The columns reach the view's bottom and each scrolls on its own; a wide board scrolls sideways
// with the bar at the bottom, or Shift and the wheel. An assignee picker narrows the cards, as GitHub's filter bar does.
// Only this project's cards are shown, so a board shared by several repositories reads as this one's. A card opens its
// issue or pull request in a side panel over the board, as GitHub's board does. An issue card lists the pull requests
// that close it, from the host's `pulls` read; this project's open the same way, others on GitHub. A card dragged to
// another column moves there at once and on GitHub through `project_board_move`, as dragging it on GitHub's board
// does; a refusal puts the board back as GitHub has it, and a read that lags behind the move keeps it where it went.
#include "screens.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The actions, from the host's `action_base` up.
enum { A_CARD, A_OPEN_GITHUB, A_FILTER_ASSIGNEE, A_CLEAR_FILTER, A_CARD_PULL };
enum { COL_W = 300, COL_MAX_W = 380, COL_GAP = 12, COL_MIN_H = 240 };
// How long a card GitHub moved is put in its new column by hand when a board read still shows it in the old one.
enum { SETTLE_SECONDS = 120 };

/// A card GitHub moved, the column it came from and the one it went to (NULL for the "No <field>" column).
typedef struct { char *id, *from, *column; time_t until; } Settling;

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
    // The board's cards are read through its view's filter, a GitHub search that lags behind a move by seconds: a
    // read just after one shows the card where it was, and the server keeps that read for 45 seconds.
    Settling *settling; size_t settling_count;
};

bool board_tab_offered(const Project *project) { return project->has_board && store_supports("project_board"); }

static void relayout(BoardTab *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}
static char *cache_key(const BoardTab *p) { return xstrfmt("project-board:%s", p->project.repo); }
// The pick is kept on disk per repository, as the other tabs' pickers are.
static char *filter_key(const BoardTab *p) { return xstrfmt("project-board-filter:%s", p->project.repo); }
static void filter_save(const BoardTab *p) {
    Json *saved = json_object();
    if (!str_empty(p->assignee)) json_set_str(saved, "assignee", p->assignee);
    char *key = filter_key(p); cache_store(g_store.cache, saved, key); free(key);
    json_free(saved);
}
static void filter_restore(BoardTab *p) {
    char *key = filter_key(p);
    Json *saved = cache_value(g_store.cache, key);
    free(key);
    set_string(&p->assignee, json_str_nonempty(json_get(saved, "assignee")));
    json_free(saved);
}
static void settled(BoardTab *p, size_t i) {
    free(p->settling[i].id); free(p->settling[i].from); free(p->settling[i].column);
    p->settling[i] = p->settling[--p->settling_count];
}
/// Puts the cards GitHub moved lately in their new columns, on a read that has them in their old ones.
static void settle(BoardTab *p, ProjectBoard *board) {
    time_t now = time(NULL);
    for (size_t i = 0; i < p->settling_count;) {
        const Settling *m = &p->settling[i];
        size_t column, card;
        bool found = project_board_find(board, m->id, &column, &card);
        int to = project_board_column(board, m->column);
        // Once a read shows it there or anywhere but where it came from (moved again since), or long enough after, the
        // read is GitHub's own.
        if (now > m->until || (found && ((int)column == to || !str_eq(board->columns[column].id, m->from)))) { settled(p, i); continue; }
        if (found && to >= 0) project_board_move(board, column, card, (size_t)to);
        i++;
    }
}
static void show(BoardTab *p, const Json *answer) {
    ProjectBoard board;
    if (!project_board_parse(answer, &board)) return;
    project_board_keep_repo(&board, p->project.repo);
    settle(p, &board);
    project_board_free(&p->board);
    p->board = board; p->has_board = true;
}

// MARK: - Loading

static void undo_move(BoardTab *p);
static void board_done(void *owner, Request *req) {
    BoardTab *p = owner;
    p->loaded = true;
    if (!req->ok) {
        // A move GitHub refused and no board to show instead: the card goes back by hand.
        if (!p->move_req) undo_move(p);
        request_error_into(&p->error, req); relayout(p); return;
    }
    if (!p->move_req) set_string(&p->moved_id, NULL);
    set_string(&p->error, NULL);
    // A carried card's arg is its place on the board shown: dropped after the new one replaces it, it would name another card.
    if (p->host->pane && pane_top(p->host->pane) == p->host) pane_cancel_carry(p->host->pane, p->base + A_CARD);
    show(p, req->result);
    // A refusal is not saved: the board last read stays the one shown the next time.
    if (p->has_board && !p->board.error) { char *key = cache_key(p); cache_store(g_store.cache, req->result, key); free(key); }
    relayout(p);
}
static void load(BoardTab *p, bool fresh) {
    if (!board_tab_offered(&p->project)) return;
    if (!p->has_board) {
        char *key = cache_key(p);
        Json *saved = cache_value(g_store.cache, key);
        free(key);
        if (saved) { show(p, saved); json_free(saved); }
    }
    request_cancel(&p->req);
    Json *args = json_object(); json_set_str(args, "repo", p->project.repo);
    if (fresh) json_set_str(args, "fresh", "1");
    store_call("project_board", args, 0, p, board_done, 0, &p->req);
}
void board_tab_set_pulls(BoardTab *p, const IssueSummary *issues, size_t issue_count, const PullSummary *pulls, size_t pull_count) {
    p->issues = issues; p->issue_count = issue_count; p->pulls = pulls; p->pull_count = pull_count;
}
// While a card is moving, the read after the move is the first.
void board_tab_open(BoardTab *p) { if (!p->loaded && !p->req && !p->move_req) load(p, false); }
void board_tab_refresh(BoardTab *p) { set_string(&p->move_error, NULL); load(p, true); relayout(p); }

// MARK: - Painting

static COLORREF option_color(const char *name, COLORREF fallback) {
    int rgb[3];
    return project_color_rgb(name, rgb) ? RGB(rgb[0], rgb[1], rgb[2]) : fallback;
}
/// A column's frame, lit while a card carried over it would land there.
typedef struct { const BoardTab *p; int index; } ColumnData;
static void paint_column(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    const ColumnData *d = it->data;
    if (d->p->drop_column == d->index) fill_round_rect(cv, rc, px(10), blend(theme.accent, theme.sidebar, 0.08), theme.accent);
    else fill_round_rect(cv, rc, px(10), theme.sidebar, theme.line);
}

/// A column's heading: its option's colour as a ring, its name and how many cards it holds.
typedef struct { char *name, *count; COLORREF color; } HeadData;
static void head_free(void *v) { HeadData *d = v; free(d->name); free(d->count); free(d); }
static void paint_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    HeadData *d = it->data;
    int cy = (rc->top + rc->bottom) / 2, x = rc->left;
    stroke_circle(cv, x + px(6), cy, px(5), d->color, px(2));
    x += px(18);
    int cw = text_width(cv, d->count, FONT_CAPTION_SEMIBOLD) + px(12);
    RECT name = { x, rc->top, rc->right - cw - px(6), rc->bottom };
    draw_text(cv, d->name, &name, FONT_SUBHEADLINE_SEMIBOLD, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int nw = text_width(cv, d->name, FONT_SUBHEADLINE_SEMIBOLD);
    int bx = x + (nw < name.right - name.left ? nw : name.right - name.left) + px(8);
    RECT bubble = { bx, cy - px(9), bx + cw, cy + px(9) };
    fill_round_rect(cv, &bubble, px(9), theme.raise, theme.raise);
    draw_text(cv, d->count, &bubble, FONT_CAPTION_SEMIBOLD, theme.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/// A card's first line: its state as a coloured dot, then where it lives, "hq-core #123".
typedef struct { char *text; COLORREF color; bool hollow; } RefData;
static void ref_free(void *v) { RefData *d = v; free(d->text); free(d); }
static void paint_ref(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    RefData *d = it->data;
    int cy = (rc->top + rc->bottom) / 2;
    if (d->hollow) stroke_circle(cv, rc->left + px(5), cy, px(4), d->color, px(1)); else fill_circle(cv, rc->left + px(5), cy, px(4), d->color);
    RECT t = { rc->left + px(15), rc->top, rc->right, rc->bottom };
    draw_text(cv, d->text, &t, FONT_MONO_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}
/// GitHub's own colours for an issue's or pull request's state: green open, purple done, grey not planned or a draft.
static COLORREF state_color(const ProjectCard *card) {
    if (str_eq(card->type, "draft") || str_eq(card->type, "redacted")) return theme.secondary;
    if (str_eq(card->state, "merged") || (str_eq(card->state, "closed") && str_eq(card->type, "issue"))) return RGB(0x89, 0x57, 0xE5);
    if (str_eq(card->state, "closed")) return theme.danger;
    return RGB(0x1A, 0x7F, 0x37);
}
static char *card_reference(const ProjectCard *card) {
    if (str_eq(card->type, "draft")) return xstrdup("Draft");
    if (str_eq(card->type, "redacted")) return xstrdup("Private item");
    const char *slash = card->repo ? strrchr(card->repo, '/') : NULL;
    const char *name = slash ? slash + 1 : card->repo;
    return name && card->number ? xstrfmt("%s #%d", name, card->number) : card->number ? xstrfmt("#%d", card->number) : xstrdup("");
}

// MARK: - Layout

static intptr_t card_arg(size_t column, size_t card) { return (intptr_t)((column << 16) | card); }
static const ProjectCard *card_at(const BoardTab *p, intptr_t arg) {
    size_t column = (size_t)arg >> 16, card = (size_t)arg & 0xFFFF;
    if (column >= p->board.column_count || card >= p->board.columns[column].card_count) return NULL;
    return &p->board.columns[column].cards[card];
}
/// A card's pull request link: the card's argument, then which of its pull requests.
static intptr_t card_pull_arg(intptr_t card, size_t pull) { return (card << 8) | (intptr_t)(pull & 0xFF); }
static BoardLink *card_pulls(const BoardTab *p, const ProjectCard *card, size_t *count) {
    return project_card_pulls(card, p->project.repo, p->issues, p->issue_count, p->pulls, p->pull_count, count);
}
static void card_pulls_free(BoardLink *links, size_t count) { for (size_t i = 0; i < count; i++) board_link_free(&links[i]); free(links); }
/// Whether a click on the card leads anywhere: to the app's own screen or to GitHub.
static bool card_opens(const ProjectCard *card) { return card->number > 0 && (card->repo || safe_web_url(card->url)); }
/// Whether the card can be dragged to another column: the server moves cards, none is on its way already, and no
/// failed move waits for the read that puts its card back (a second move would cancel that read).
static bool card_moves(const BoardTab *p, const ProjectCard *card) {
    return !p->move_req && !p->moved_id && !str_empty(card->id) && !str_eq(card->type, "redacted") && store_supports("project_board_move");
}

static void layout_card(BoardTab *p, Doc *doc, int x, int w, const ProjectCard *card, intptr_t arg) {
    bool opens = card_opens(card);
    int box = doc_box_begin(doc, x, w, px(10), theme.elevated, theme.line, px(8));
    int ix = x + px(12), iw = w - px(24);
    RefData *ref = xcalloc(1, sizeof *ref);
    ref->text = card_reference(card); ref->color = state_color(card); ref->hollow = str_eq(card->type, "draft");
    doc_custom(doc, ix, iw, font_height(doc->cv, FONT_MONO_CAPTION2) + px(2), paint_ref, ref, ref_free, 0, 0);
    doc_space(doc, px(4));
    doc_text(doc, ix, iw, card->title ? card->title : "Untitled", FONT_FOOTNOTE_SEMIBOLD, theme.ink, DT_LEFT | DT_WORDBREAK);
    // The fields as GitHub's cards show them: single-selects as chips in their option's colour, the rest named.
    if (card->field_count) {
        BadgeSpec *specs = xcalloc(card->field_count, sizeof *specs);
        char **texts = xcalloc(card->field_count, sizeof *texts);
        for (size_t i = 0; i < card->field_count; i++) {
            const ProjectField *f = &card->fields[i];
            texts[i] = f->color ? xstrdup(f->value) : xstrfmt("%s: %s", f->name, f->value);
            specs[i].text = texts[i]; specs[i].color = option_color(f->color, theme.secondary); specs[i].chip = f->color != NULL;
        }
        doc_space(doc, px(6));
        doc_badges(doc, ix, iw, specs, card->field_count, theme.elevated);
        str_array_free(texts, card->field_count);
        free(specs);
    }
    if (card->label_count) { doc_space(doc, px(6)); doc_label_chips(doc, ix, iw, card->labels, card->label_count, theme.elevated); }
    if (card->has_parent) { doc_space(doc, px(4)); doc_linked_row(doc, ix, iw, &card->parent, p->project.repo, 0, 0); }
    // The pull requests that close it, each opening on its own.
    size_t pn; BoardLink *pulls = card_pulls(p, card, &pn);
    for (size_t i = 0; i < pn && i < 0x100; i++) {
        bool here = !board_link_is_foreign(&pulls[i], p->project.repo) && store_supports("pull");
        doc_space(doc, px(4));
        doc_linked_row(doc, ix, iw, &pulls[i], p->project.repo, here || safe_web_url(pulls[i].url) ? p->base + A_CARD_PULL : 0, card_pull_arg(arg, i));
    }
    card_pulls_free(pulls, pn);
    // Who has it and when it was opened.
    Str meta; str_init(&meta);
    if (card->assignee_count) { char *who = people(card->assignees, card->assignee_count, 3); str_appendz(&meta, who); free(who); }
    if (card->has_created) { char *when = format_date_abbrev(card->created_at); str_appendf(&meta, "%sopened %s", meta.len ? " \xC2\xB7 " : "", when); free(when); }
    if (meta.len) { doc_space(doc, px(6)); doc_text(doc, ix, iw, meta.data, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS); }
    str_free(&meta);
    doc_box_end(doc, box, px(10));
    if (opens) doc_box_action(doc, box, p->base + A_CARD, arg);
    if (card_moves(p, card)) {
        Item *it = doc_item(doc, box);
        it->drag = true;
        // A card that opens nothing still needs an action to be pressed, and so carried.
        if (!opens) { it->action = p->base + A_CARD; it->arg = arg; }
    }
}

/// A column down to `bottom`: its heading stays put and its cards scroll beneath it on their own.
static void layout_column(BoardTab *p, Doc *doc, int x, int w, size_t index, int bottom) {
    const ProjectColumn *c = &p->board.columns[index];
    int ix = x + px(10), iw = w - px(20);
    // Filtered, the count and the totals are the shown cards', as GitHub's are.
    bool filtered = !str_empty(p->assignee);
    double *totals = xcalloc(c->sum_count + 1, sizeof *totals);
    int count = c->count;
    if (filtered) count = project_column_matching(c, p->assignee, totals);
    else for (size_t i = 0; i < c->sum_count; i++) totals[i] = c->sums[i].value;
    doc_space(doc, px(10));
    HeadData *head = xcalloc(1, sizeof *head);
    head->name = xstrdup(c->name); head->count = xstrfmt("%d", count); head->color = option_color(c->color, theme.secondary);
    doc_custom(doc, ix, iw, px(24), paint_head, head, head_free, 0, 0);
    // Each number field totalled, "Story Points: 21", as GitHub's column footer words it.
    if (c->sum_count) {
        Str sums; str_init(&sums);
        for (size_t i = 0; i < c->sum_count; i++) { char *n = project_sum_text(totals[i]); str_appendf(&sums, "%s%s: %s", i ? " \xC2\xB7 " : "", c->sums[i].name, n); free(n); }
        doc_space(doc, px(2));
        doc_text(doc, ix, iw, sums.data, FONT_CAPTION2, theme.secondary, DT_LEFT | DT_WORDBREAK);
        str_free(&sums);
    }
    free(totals);
    doc_space(doc, px(10));
    int first = (int)doc->count, cards_top = doc->y;
    bool any = false;
    for (size_t i = 0; i < c->card_count; i++) {
        if (!project_card_assigned(&c->cards[i], p->assignee)) continue;
        if (any) doc_space(doc, px(8));
        layout_card(p, doc, ix, iw, &c->cards[i], card_arg(index, i));
        any = true;
    }
    if (!any) doc_text(doc, ix, iw, "No items", FONT_CAPTION, theme.tertiary, DT_LEFT | DT_SINGLELINE);
    doc_space(doc, px(10));
    // Inside the column's border, clear of its rounded bottom corners.
    RECT view = { x + 1, cards_top, x + w - 1, bottom - px(8) };
    if (view.bottom < view.top) view.bottom = view.top;
    doc_region(doc, first, (int)doc->count, &view, doc->y);
}
/// How many cards the board shows: every item on it, or those the picked assignee has.
static int shown_items(const BoardTab *p) {
    int items = 0;
    for (size_t i = 0; i < p->board.column_count; i++)
        items += str_empty(p->assignee) ? p->board.columns[i].count : project_column_matching(&p->board.columns[i], p->assignee, NULL);
    return items;
}

void board_tab_layout(BoardTab *p, Doc *doc, int w) {
    board_tab_open(p);
    const ProjectBoard *b = &p->board;
    if (p->error) { doc_notice(doc, 0, w, p->error); doc_space(doc, px(10)); }
    if (p->move_error) { doc_notice(doc, 0, w, p->move_error); doc_space(doc, px(10)); }
    if (!p->has_board) {
        if (!p->error) doc_loading(doc, 0, w, "Loading the board\xE2\x80\xA6");
        return;
    }
    if (b->error) {
        doc_notice_box(doc, 0, w, b->error);
        doc_space(doc, px(6));
        doc_text(doc, 0, w, "Reading a board needs Projects: read on the server's GitHub token (read:project on a classic token), and a project and view GitHub can find. The board is named in Settings \xE2\x86\x92 Projects.",
                 FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        doc_space(doc, px(10));
    }
    if (!str_empty(b->filter)) {
        char *line = xstrfmt("Filtered by %s", b->filter);
        doc_text(doc, 0, w, line, FONT_MONO_CAPTION2, theme.secondary, DT_LEFT | DT_WORDBREAK);
        free(line);
        doc_space(doc, px(8));
    }
    if (b->truncated) { doc_text(doc, 0, w, "Only the board's first 2,000 items are shown.", FONT_CAPTION, theme.warn, DT_LEFT | DT_WORDBREAK); doc_space(doc, px(8)); }
    if (!str_empty(p->assignee) && b->column_count) {
        int total = 0;
        for (size_t i = 0; i < b->column_count; i++) total += b->columns[i].count;
        char *text = xstrfmt("Showing %d of %d", shown_items(p), total);
        int y = doc->y;
        int cw = text_width(doc->cv, "Clear filter", FONT_CAPTION) + px(8);
        RECT tr = { 0, y, w - cw - px(8), y + px(20) };
        doc_text_at(doc, &tr, text, FONT_CAPTION, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT cr = { w - cw, y, w, tr.bottom };
        int ci = doc_text_at(doc, &cr, "Clear filter", FONT_CAPTION, theme.accent, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        doc_item(doc, ci)->action = p->base + A_CLEAR_FILTER; doc_item(doc, ci)->hand = true;
        doc->y = tr.bottom + px(8);
        free(text);
    }
    size_t n = b->column_count;
    if (!n) {
        if (!b->error) doc_empty_state(doc, 0, w, 0xE8FD, "Nothing on the board", "No item on this board matches its view's filter.");
        return;
    }
    // The columns share the width when they fit, and keep a readable width and scroll sideways when they do not.
    int gap = px(COL_GAP), cw = px(COL_W);
    int fit = (w - gap * (int)(n - 1)) / (int)n;
    if (fit > cw) cw = fit < px(COL_MAX_W) ? fit : px(COL_MAX_W);
    // The columns reach down to the view's bottom, above the sideways bar, so the page itself stays put; in a window too
    // short for that they keep a readable height and the page scrolls instead.
    int top = doc->y, view_h = 0;
    if (p->host->pane) { RECT v = pane_content_rect(p->host->pane); view_h = v.bottom - v.top; }
    int bottom = view_h - px(26);
    if (bottom < top + px(COL_MIN_H)) bottom = top + px(COL_MIN_H);
    p->columns_top = top; p->column_w = cw; p->column_step = cw + gap;
    for (size_t i = 0; i < n; i++) {
        int x = (int)i * (cw + gap);
        RECT rc = { x, top, x + cw, bottom };
        ColumnData *data = xcalloc(1, sizeof *data);
        data->p = p; data->index = (int)i;
        Item *frame = doc_item(doc, doc_add(doc, &rc, paint_column));
        frame->data = data; frame->free_data = free;
        doc->y = top;
        layout_column(p, doc, x, cw, i, bottom);
    }
    doc->y = bottom;
}

void board_tab_header(BoardTab *p, HeaderInfo *info) {
    const ProjectBoard *b = &p->board;
    if (p->has_board && (b->title || b->view_name)) {
        int items = shown_items(p);
        const char *title = b->title ? b->title : "Project", *view = b->view_name;
        if (view) snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %s \xC2\xB7 %d item%s", title, view, items, items == 1 ? "" : "s");
        else snprintf(info->subtitle, sizeof info->subtitle, "%s \xC2\xB7 %d item%s", title, items, items == 1 ? "" : "s");
    }
    // The assignee picker, as the other tabs' pickers are.
    if (p->has_board && b->column_count && info->button_count < HEADER_BUTTONS) {
        HeaderButton *f = &info->buttons[info->button_count++];
        const char *pick = str_empty(p->assignee) ? "All assignees" : str_eq(p->assignee, PROJECT_NO_ASSIGNEE) ? "No assignee" : p->assignee;
        snprintf(f->label, sizeof f->label, "%s \xE2\x96\xBE", pick);
        f->glyph = 0xE716; f->action = p->base + A_FILTER_ASSIGNEE; f->enabled = true; f->tip = "Show the cards of one assignee";
    }
    const char *url = safe_web_url(b->view_url) ? b->view_url : b->url;
    if (safe_web_url(url) && info->button_count < HEADER_BUTTONS) {
        HeaderButton *g = &info->buttons[info->button_count++];
        g->glyph = 0xE8A7; g->action = p->base + A_OPEN_GITHUB; g->enabled = true; g->tip = "Open the board on GitHub";
    }
}

// MARK: - Actions

static void open_card(BoardTab *p, const ProjectCard *card) {
    bool here = card->repo && str_ieq(card->repo, p->project.repo);
    if (here && str_eq(card->type, "issue") && store_supports("issue")) {
        // The issue screen reads the rest itself; the card gives it what to show until it has.
        IssueSummary issue = { 0 };
        issue.number = card->number; issue.title = card->title; issue.url = card->url; issue.author = card->author;
        issue.assignees = card->assignees; issue.assignee_count = card->assignee_count;
        issue.labels = card->labels; issue.label_count = card->label_count;
        issue.has_created = card->has_created; issue.created_at = card->created_at;
        if (card->has_parent) { issue.has_parent = true; issue.parent = card->parent; }
        app_set_overlay(issue_detail_screen_new(&p->project, &issue));
        return;
    }
    if (here && str_eq(card->type, "pull") && store_supports("pull")) { app_set_overlay(pull_detail_screen_new(&p->project, card->number, NULL, NULL)); return; }
    if (safe_web_url(card->url)) open_web_url(card->url);
}

/// A pull request of this project opens in the side panel, as its card would; any other on GitHub.
static void open_card_pull(BoardTab *p, const ProjectCard *card, size_t index) {
    size_t n; BoardLink *pulls = card_pulls(p, card, &n);
    if (index < n) {
        const BoardLink *l = &pulls[index];
        if (!board_link_is_foreign(l, p->project.repo) && store_supports("pull"))
            app_set_overlay(pull_detail_screen_new(&p->project, l->number, NULL, pulls_find(p->pulls, p->pull_count, l->number)));
        else if (safe_web_url(l->url)) open_web_url(l->url);
    }
    card_pulls_free(pulls, n);
}

static void set_assignee(BoardTab *p, const char *assignee) {
    set_string(&p->assignee, str_empty(assignee) ? NULL : assignee);
    filter_save(p);
    relayout(p);
}
/// The picker's menu: everyone, then each assignee with how many cards they have.
static void pick_assignee(BoardTab *p, POINT pt) {
    size_t count; FilterOption *options = project_board_assignees(&p->board, p->assignee, &count);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (str_empty(p->assignee) ? MF_CHECKED : 0), 1, L"All assignees");
    for (size_t i = 0; i < count; i++) {
        char *label = xstrfmt("%s (%d)", options[i].text, options[i].count); wchar_t *wl = utf8_to_wide(label);
        bool picked = !str_empty(p->assignee) && str_ieq(p->assignee, options[i].value);
        AppendMenuW(menu, MF_STRING | (picked ? MF_CHECKED : 0), (UINT_PTR)(2 + i), wl);
        free(label); free(wl);
    }
    int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, pane_hwnd(p->host->pane), NULL);
    DestroyMenu(menu);
    if (chosen == 1) set_assignee(p, NULL);
    else if (chosen >= 2 && (size_t)(chosen - 2) < count) set_assignee(p, options[chosen - 2].value);
    filter_options_free(options, count);
}

bool board_tab_action(BoardTab *p, int action, intptr_t arg, POINT pt) {
    if (action < p->base || action >= p->base + BOARD_TAB_ACTIONS) return false;
    switch (action - p->base) {
    case A_CARD: { const ProjectCard *card = card_at(p, arg); if (card) open_card(p, card); break; }
    case A_CARD_PULL: { const ProjectCard *card = card_at(p, arg >> 8); if (card) open_card_pull(p, card, (size_t)(arg & 0xFF)); break; }
    case A_FILTER_ASSIGNEE: if (p->host->pane) pick_assignee(p, pt); break;
    case A_CLEAR_FILTER: set_assignee(p, NULL); break;
    case A_OPEN_GITHUB: { const char *url = safe_web_url(p->board.view_url) ? p->board.view_url : p->board.url; if (safe_web_url(url)) open_web_url(url); break; }
    }
    return true;
}

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
        for (size_t i = 0; i < p->settling_count; i++) if (str_eq(p->settling[i].id, p->moved_id)) { settled(p, i); break; }
        p->settling = xrealloc(p->settling, (p->settling_count + 1) * sizeof *p->settling);
        Settling *m = &p->settling[p->settling_count++];
        m->id = p->moved_id; p->moved_id = NULL;
        m->from = p->moved_off; p->moved_off = NULL;
        m->column = p->moved_to; p->moved_to = NULL;
        m->until = time(NULL) + SETTLE_SECONDS;
    }
    load(p, !req->ok);
    relayout(p);
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
    relayout(p);
}
/// Puts the card a failed move carried back where it was, when no board read has replaced the guess.
static void undo_move(BoardTab *p) {
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

// MARK: - Lifetime

BoardTab *board_tab_new(const Project *project, Screen *host, int action_base) {
    BoardTab *p = xcalloc(1, sizeof *p);
    project_copy(&p->project, project);
    p->host = host; p->base = action_base; p->drop_column = -1;
    filter_restore(p);
    return p;
}
void board_tab_free(BoardTab *p) {
    if (!p) return;
    request_cancel(&p->req); request_cancel(&p->move_req);
    // The side panel showed one of this board's cards. A board that was never
    // shown (a duplicate `app_show_detail` throws away) owns no panel.
    if (p->host->pane) app_set_overlay(NULL);
    project_board_free(&p->board);
    project_free(&p->project); free(p->error); free(p->move_error); free(p->moved_id); free(p->moved_off); free(p->moved_to); free(p->assignee);
    while (p->settling_count) settled(p, p->settling_count - 1);
    free(p->settling);
    free(p);
}
