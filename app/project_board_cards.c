#include "project_board_internal.h"

/// A column's frame, lit while a card carried over it would land there.
typedef struct { const BoardTab *p; int index; } ColumnData;

/// A column's heading: its option's colour as a ring, its name and how many cards it holds.
typedef struct { char *name, *count; COLORREF color; } HeadData;

/// A card's first line: its state as a coloured dot, then where it lives, "hq-core #123".
typedef struct { char *text; COLORREF color; bool hollow; } RefData;

static COLORREF option_color(const char *name, COLORREF fallback);
static void paint_column(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void head_free(void *v);
static void paint_head(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void ref_free(void *v);
static void paint_ref(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static COLORREF state_color(const ProjectCard *card);
static char *card_reference(const ProjectCard *card);
static intptr_t card_arg(size_t column, size_t card);
static intptr_t card_pull_arg(intptr_t card, size_t pull);
static bool card_opens(const ProjectCard *card);
static bool card_moves(const BoardTab *p, const ProjectCard *card);
static void layout_card(BoardTab *p, Doc *doc, int x, int w, const ProjectCard *card, intptr_t arg);
static void layout_column(BoardTab *p, Doc *doc, int x, int w, size_t index, int bottom);
static int shown_items(const BoardTab *p);

// MARK: - Painting

static COLORREF option_color(const char *name, COLORREF fallback) {
    int rgb[3];
    return project_color_rgb(name, rgb) ? RGB(rgb[0], rgb[1], rgb[2]) : fallback;
}

static void paint_column(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    const ColumnData *d = it->data;
    if (d->p->drop_column == d->index) fill_round_rect(cv, rc, px(10), blend(theme.accent, theme.sidebar, 0.08), theme.accent);
    else fill_round_rect(cv, rc, px(10), theme.sidebar, theme.line);
}

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

const ProjectCard *project_board_card_at(const BoardTab *p, intptr_t arg) {
    size_t column = (size_t)arg >> 16, card = (size_t)arg & 0xFFFF;
    if (column >= p->board.column_count || card >= p->board.columns[column].card_count) return NULL;
    return &p->board.columns[column].cards[card];
}

/// A card's pull request link: the card's argument, then which of its pull requests.
static intptr_t card_pull_arg(intptr_t card, size_t pull) { return (card << 8) | (intptr_t)(pull & 0xFF); }

BoardLink *project_board_card_pulls(const BoardTab *p, const ProjectCard *card, size_t *count) {
    return project_card_pulls(card, p->project.repo, p->issues, p->issue_count, p->pulls, p->pull_count, count);
}

void project_board_card_pulls_free(BoardLink *links, size_t count) { for (size_t i = 0; i < count; i++) board_link_free(&links[i]); free(links); }

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
    size_t pn; BoardLink *pulls = project_board_card_pulls(p, card, &pn);
    for (size_t i = 0; i < pn && i < 0x100; i++) {
        bool here = !board_link_is_foreign(&pulls[i], p->project.repo) && store_supports("pull");
        doc_space(doc, px(4));
        doc_linked_row(doc, ix, iw, &pulls[i], p->project.repo, here || safe_web_url(pulls[i].url) ? p->base + A_CARD_PULL : 0, card_pull_arg(arg, i));
    }
    project_board_card_pulls_free(pulls, pn);
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
