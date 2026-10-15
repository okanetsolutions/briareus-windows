#include "project_db_internal.h"

// MARK: - The tree

typedef struct { char *name, *sub; int depth; wchar_t glyph; bool chevron, open, selected, dim; } NodeData;

typedef struct { ProjectDb *p; size_t row; } GridRowData;

static void node_free(void *v);
static void paint_node(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void add_node(ProjectDb *p, Doc *doc, int x, int w, NodeData *d, int action, intptr_t arg);
static void node_note(Doc *doc, int x, int w, int depth, const char *text, bool error);
static void layout_tree(ProjectDb *p, Doc *doc, int x, int w);
static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void paint_grid_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc);
static void measure_columns(ProjectDb *p, Canvas *cv);
static void layout_grid(ProjectDb *p, Doc *doc, int x, int w);

static void node_free(void *v) { NodeData *d = v; free(d->name); free(d->sub); free(d); }

static void paint_node(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    NodeData *d = it->data;
    if (d->selected || doc_item_hovered(doc, it)) fill_round_rect(cv, rc, px(6), theme.raise, theme.raise);
    int x = rc->left + px(4) + d->depth * px(16);
    RECT chevron = { x, rc->top, x + px(14), d->sub ? rc->top + px(46) : rc->bottom };
    if (d->chevron) draw_glyph(cv, d->open ? 0xE70D : 0xE76C, &chevron, FONT_ICON_SMALL, theme.muted);
    RECT icon = { chevron.right + px(2), chevron.top, chevron.right + px(22), chevron.bottom };
    draw_glyph(cv, d->glyph, &icon, FONT_ICON_SMALL, d->dim ? theme.secondary : theme.accent);
    int tx = icon.right + px(6), right = rc->right - px(8);
    if (!d->sub) {
        RECT t = { tx, rc->top, right, rc->bottom };
        draw_text(cv, d->name, &t, d->selected ? FONT_FOOTNOTE_SEMIBOLD : FONT_FOOTNOTE, theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    int top = rc->top + px(5);
    RECT t = { tx, top, right, top + px(20) };
    draw_text(cv, d->name, &t, FONT_SUBHEADLINE, d->dim ? theme.secondary : theme.ink, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sub = { tx, top + px(20), right, top + px(36) };
    draw_text(cv, d->sub, &sub, FONT_CAPTION2, theme.muted, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void add_node(ProjectDb *p, Doc *doc, int x, int w, NodeData *d, int action, intptr_t arg) {
    int h = d->sub ? px(46) : px(26);
    doc_item(doc, doc_custom(doc, x, w, h, paint_node, d, node_free, p->base + action, arg))->hand = true;
}

/// A line under an open node: loading, an error or nothing there, indented to its children.
static void node_note(Doc *doc, int x, int w, int depth, const char *text, bool error) {
    int ix = x + px(4) + depth * px(16) + px(22);
    if (error) doc_text(doc, ix, w - (ix - x), text, FONT_CAPTION, theme.danger, DT_LEFT | DT_WORDBREAK);
    else doc_text(doc, ix, w - (ix - x), text, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    doc_space(doc, px(4));
}

static void layout_tree(ProjectDb *p, Doc *doc, int x, int w) {
    if (p->error) { doc_notice(doc, x, w, p->error); doc_space(doc, px(8)); }
    if (!p->loaded) { doc_loading(doc, x, w, "Loading servers\xE2\x80\xA6"); return; }
    for (size_t s = 0; s < p->server_count; s++) {
        ServerNode *n = &p->servers[s];
        NodeData *d = xcalloc(1, sizeof *d);
        d->name = project_db_server_name(n->row);
        const char *user = json_str(json_get(n->row, "username")), *host = json_str(json_get(n->row, "host"));
        d->sub = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", project_db_server_port(n->row));
        d->glyph = 0xE1D3; d->chevron = true; d->open = n->open;
        d->dim = !json_bool_is(json_get(n->row, "hasDbCredentials"), true);
        add_node(p, doc, x, w, d, A_SERVER, project_db_pack((int)s, -1, -1));
        if (!n->open) continue;
        if (n->loading) node_note(doc, x, w, 1, "Connecting\xE2\x80\xA6", false);
        if (n->error) node_note(doc, x, w, 1, n->error, true);
        if (n->loaded && !n->db_count) node_note(doc, x, w, 1, "No databases.", false);
        for (size_t di = 0; di < n->db_count; di++) {
            DbNode *db = &n->dbs[di];
            NodeData *dd = xcalloc(1, sizeof *dd);
            dd->name = xstrdup(db->name); dd->depth = 1; dd->glyph = 0xE8B7; dd->chevron = true; dd->open = db->open;
            add_node(p, doc, x, w, dd, A_DB, project_db_pack((int)s, (int)di, -1));
            if (!db->open) continue;
            if (db->loading) node_note(doc, x, w, 2, "Loading tables\xE2\x80\xA6", false);
            if (db->error) node_note(doc, x, w, 2, db->error, true);
            if (db->loaded && !db->table_count) node_note(doc, x, w, 2, "No tables.", false);
            for (size_t ti = 0; ti < db->table_count; ti++) {
                NodeData *td = xcalloc(1, sizeof *td);
                td->name = xstrdup(db->tables[ti]); td->depth = 2; td->glyph = 0xE8FD;
                td->selected = (int)s == p->sel_server && (int)di == p->sel_db && (int)ti == p->sel_table;
                add_node(p, doc, x, w, td, A_TABLE, project_db_pack((int)s, (int)di, (int)ti));
            }
        }
    }
    if (!p->server_count && !p->error)
        doc_text(doc, x + px(6), w - px(12), "No SSH servers for this project. Register one, with its database login, under \xE2\x9A\x99 Settings.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
}

// MARK: - The grid

/// The rule beside the tree spans the tree's window, wherever the page and the tree have scrolled it.
static void paint_rule(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    int dy = rc->top - it->rc.top;
    RECT r = { rc->left, doc->sticky_view.top + dy, rc->right, doc->sticky_view.bottom + dy };
    fill_rect(cv, &r, theme.line);
}

static void paint_grid_row(Doc *doc, Item *it, Canvas *cv, const RECT *rc) {
    (void)doc;
    GridRowData *d = it->data;
    ProjectDb *p = d->p;
    if (d->row >= p->grid.rows || !p->widths) return;
    bool header = d->row == 0;
    if (header) fill_rect(cv, rc, theme.raise);
    else if (d->row % 2 == 0) fill_rect(cv, rc, blend(theme.raise, theme.canvas, 0.35));
    canvas_clip(cv, rc);
    int x = rc->left;
    // The row's number first, as DBeaver's grid shows it.
    char num[16]; snprintf(num, sizeof num, "%zu", d->row);
    int nw = px(44);
    RECT nr = { x, rc->top, x + nw - px(8), rc->bottom };
    if (!header) draw_text(cv, num, &nr, FONT_CAPTION, theme.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    x += nw;
    for (size_t c = 0; c < p->grid.cols && x < rc->right; c++) {
        RECT line = { x, rc->top, x + 1, rc->bottom };
        fill_rect(cv, &line, theme.line);
        RECT t = { x + px(6), rc->top, x + p->widths[c] - px(6), rc->bottom };
        const char *v = sql_cell(&p->grid, d->row, c);
        bool null = !header && str_eq(v, "NULL");
        draw_text(cv, v, &t, header ? FONT_CAPTION_SEMIBOLD : FONT_CAPTION, header ? theme.ink : null ? theme.muted : theme.text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        x += p->widths[c];
    }
    canvas_unclip(cv);
    RECT bottom = { rc->left, rc->bottom - 1, rc->right, rc->bottom };
    fill_rect(cv, &bottom, theme.line);
}

static void measure_columns(ProjectDb *p, Canvas *cv) {
    if (p->widths || !p->grid.cols) return;
    p->widths = xcalloc(p->grid.cols, sizeof *p->widths);
    for (size_t c = 0; c < p->grid.cols; c++) {
        int w = text_width(cv, sql_cell(&p->grid, 0, c), FONT_CAPTION_SEMIBOLD);
        for (size_t r = 1; r < p->grid.rows; r++) { int cw = text_width(cv, sql_cell(&p->grid, r, c), FONT_CAPTION); if (cw > w) w = cw; if (w >= px(CELL_MAX_W)) break; }
        if (w > px(CELL_MAX_W)) w = px(CELL_MAX_W);
        p->widths[c] = w + px(12);
    }
}

static void layout_grid(ProjectDb *p, Doc *doc, int x, int w) {
    DbNode *db = project_db_db_at(p, p->sel_server, p->sel_db);
    if (!db || p->sel_table < 0 || (size_t)p->sel_table >= db->table_count) {
        doc_space(doc, px(40));
        doc_empty_state(doc, x, w, 0xE1D3, "No table selected",
                        "Click a server on the left to see its databases, a database to see its tables, and a table to see its rows here. "
                        "The queries run on the server's own mysql client over SSH from this PC, with the database login stored for the server.");
        return;
    }
    char *title = xstrfmt("%s.%s", db->name, db->tables[p->sel_table]);
    int top = doc->y;
    doc_text(doc, x, w - px(120), title, FONT_HEADLINE, theme.ink, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    free(title);
    int after = doc->y;
    doc->y = top;
    int bw = px(96);
    doc_button(doc, x + w - bw, bw, "Reload", BUTTON_BORDERED, p->base + A_RELOAD, 0, !p->grid_loading);
    if (doc->y < after) doc->y = after;
    doc_space(doc, px(6));
    if (p->grid_loading) { doc_loading(doc, x, w, "Running the query\xE2\x80\xA6"); return; }
    if (p->grid_error) { doc_notice(doc, x, w, p->grid_error); return; }
    size_t rows = p->grid.rows ? p->grid.rows - 1 : 0;
    char *count = rows >= ROW_LIMIT ? xstrfmt("The first %d rows", ROW_LIMIT) : xstrfmt("%zu row%s", rows, rows == 1 ? "" : "s");
    doc_text(doc, x, w, count, FONT_CAPTION, theme.muted, DT_LEFT | DT_SINGLELINE);
    free(count);
    doc_space(doc, px(8));
    if (!p->grid.rows) return;
    measure_columns(p, doc->cv);
    for (size_t r = 0; r < p->grid.rows; r++) {
        GridRowData *d = xcalloc(1, sizeof *d);
        d->p = p; d->row = r;
        doc_custom(doc, x, w, px(24), paint_grid_row, d, free, 0, 0);
    }
}

void project_db_layout(ProjectDb *p, Doc *doc, int w) {
    if (!project_db_offered()) {
        doc_text(doc, 0, w, "The SSH servers and their database logins are read from Settings, which needs an Admin token on a server that stores database logins. Create one on the web dashboard under Settings \xE2\x86\x92 Devices and clients and connect with it.", FONT_FOOTNOTE, theme.muted, DT_WORDBREAK);
        return;
    }
    project_db_load(p);
    // The tabs stay at the top; beneath them the tree and the grid scroll apart: the grid with the page, the tree on
    // its own inside a window the view's height, as the Files tab's tree does.
    doc_pin(doc, (int)doc->count, doc->y);
    RECT view = pane_content_rect(p->host->pane);
    int top = doc->y, room = (view.bottom - view.top) - top - px(14);
    if (room < 0) room = 0;
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    int tree_first = (int)doc->count;
    layout_tree(p, doc, 0, lw);
    // An unpainted spacer keeps the tree's window the view's height when the tree is shorter. It takes the unclamped
    // room, so in a short pane the group never outgrows its window; the px(240) minimum applies to the page alone.
    RECT spacer = { lw + px(8), top, lw + px(9), top + room };
    doc_add(doc, &spacer, NULL);
    int tree_last = (int)doc->count;
    doc->y = top;
    int rx = lw + px(18), rw = w - rx;
    layout_grid(p, doc, rx, rw);
    int least = room < px(240) ? px(240) : room;
    if (doc->y < top + least) doc->y = top + least;
    // A rule between the tree and the grid, outside the tree's group so it does not scroll with the tree: it spans
    // the whole page and paints only the tree's window.
    RECT rule = { lw + px(8), top, lw + px(9), doc->y };
    doc_add(doc, &rule, paint_rule);
    doc_sticky(doc, tree_first, tree_last, doc->y);
}
