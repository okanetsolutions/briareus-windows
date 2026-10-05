// A project's Database tab, beside its SSH and SFTP sessions, laid out as a database browser: the project's SSH servers
// down the left as a tree (a server opens to its databases, a database to its tables), and the table clicked on the right
// as a grid of its first rows. Nothing goes through the Briareus server but the database login stored with the SSH server
// (GET …/db-credentials): every query is the server's own `mysql` client run over SSH from this PC (ssh_query.c).
#include "canvas.h"
#include "screens.h"
#include "ssh_query.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The actions, from the host's `action_base` up.
enum { A_SERVER, A_DB, A_TABLE, A_RELOAD };
enum { LIST_W = 300, ROW_LIMIT = 200, CELL_MAX_W = 260 };
enum { Q_DATABASES, Q_TABLES, Q_ROWS };

typedef struct {
    char *name;
    bool open, loading, loaded;
    char *error;
    char **tables; size_t table_count;
} DbNode;
typedef struct {
    Json *row;             // the SshServer
    Json *login;           // its DbCredentials, once read
    Request *req_login;
    bool open, loading, loaded;
    char *error;
    DbNode *dbs; size_t db_count;
} ServerNode;

struct ProjectDb {
    char *repo;
    Screen *host;
    int base;
    ServerNode *servers; size_t server_count;
    bool loaded;
    char *error;
    Request *req;
    // The table on show on the right: indices into the tree, -1 for none.
    int sel_server, sel_db, sel_table;
    SqlTable grid;
    bool grid_loading;
    char *grid_error;
    int *widths;           // each column's width in pixels, measured once per answer
};

bool project_db_offered(void) { return store_supports("settings_ssh_servers") && store_supports("ssh_db_credentials"); }

// A tree position packed into an action's argument, or a query's tag (with its kind on top).
static intptr_t pack(int s, int d, int t) { return ((intptr_t)s << 40) | ((intptr_t)(d + 1) << 20) | (intptr_t)(t + 1); }
static void unpack(intptr_t v, int *s, int *d, int *t) {
    *s = (int)((v >> 40) & 0xFFFF); *d = (int)((v >> 20) & 0xFFFFF) - 1; *t = (int)(v & 0xFFFFF) - 1;
}
static intptr_t query_tag(int kind, int s, int d, int t) { return ((intptr_t)kind << 56) | pack(s, d, t); }

static char *server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}
/// Its id, which is a timestamp-sized number, past an int.
static double server_id(const Json *row) { double id = 0; json_num(json_get(row, "id"), &id); return id; }
static int server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }
static ServerNode *server_at(ProjectDb *p, int s) { return s >= 0 && (size_t)s < p->server_count ? &p->servers[s] : NULL; }
static DbNode *db_at(ProjectDb *p, int s, int d) { ServerNode *n = server_at(p, s); return n && d >= 0 && (size_t)d < n->db_count ? &n->dbs[d] : NULL; }

static void relayout(ProjectDb *p) {
    if (!p->host->pane) return;
    pane_relayout(p->host->pane);
    pane_header_changed(p->host->pane);
}

static void grid_clear(ProjectDb *p) {
    sql_table_free(&p->grid);
    free(p->widths); p->widths = NULL;
    set_string(&p->grid_error, NULL);
    p->grid_loading = false;
}
static void db_free(DbNode *d) {
    free(d->name); free(d->error);
    for (size_t i = 0; i < d->table_count; i++) free(d->tables[i]);
    free(d->tables);
}
static void server_free(ServerNode *n) {
    request_cancel(&n->req_login);
    json_free(n->row); json_free(n->login); free(n->error);
    for (size_t i = 0; i < n->db_count; i++) db_free(&n->dbs[i]);
    free(n->dbs);
}
static void servers_clear(ProjectDb *p) {
    ssh_query_cancel(p);
    for (size_t i = 0; i < p->server_count; i++) server_free(&p->servers[i]);
    free(p->servers); p->servers = NULL; p->server_count = 0;
    p->sel_server = p->sel_db = p->sel_table = -1;
    grid_clear(p);
}

// MARK: - Queries

static void query_done(void *ctx, intptr_t tag, bool ok, const char *out);
/// Runs `sql` on server `s` with its database login, the answer coming back to query_done under `tag`.
static bool run(ProjectDb *p, int s, const char *sql, intptr_t tag, char **error) {
    ServerNode *n = server_at(p, s);
    const Json *row = n->row, *login = n->login;
    char *key = xstrfmt("db:%.0f", server_id(row)), *name = server_name(row);
    TermTarget target = { key, p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), server_port(row) };
    SqlLogin sl = { json_str(json_get(login, "host")), json_int_or(json_get(login, "port"), 3306), json_str(json_get(login, "username")), json_str(json_get(login, "password")) };
    bool started = ssh_query_run(&target, &sl, sql, query_done, p, tag, error);
    free(key); free(name);
    return started;
}
/// The server's databases, once its login is known.
static void load_databases(ProjectDb *p, int s) {
    ServerNode *n = server_at(p, s);
    char *error = NULL;
    n->loading = true;
    if (!run(p, s, "SHOW DATABASES;", query_tag(Q_DATABASES, s, -1, -1), &error)) { n->loading = false; set_string(&n->error, error); }
    free(error);
}
static void load_tables(ProjectDb *p, int s, int d) {
    DbNode *db = db_at(p, s, d);
    char *ident = sql_ident(db->name), *sql = xstrfmt("SHOW TABLES FROM %s;", ident), *error = NULL;
    db->loading = true;
    if (!run(p, s, sql, query_tag(Q_TABLES, s, d, -1), &error)) { db->loading = false; set_string(&db->error, error); }
    free(ident); free(sql); free(error);
}
static void load_rows(ProjectDb *p) {
    DbNode *db = db_at(p, p->sel_server, p->sel_db);
    if (!db || p->sel_table < 0 || (size_t)p->sel_table >= db->table_count) return;
    grid_clear(p);
    char *d = sql_ident(db->name), *t = sql_ident(db->tables[p->sel_table]);
    char *sql = xstrfmt("SELECT * FROM %s.%s LIMIT %d;", d, t, ROW_LIMIT), *error = NULL;
    p->grid_loading = true;
    if (!run(p, p->sel_server, sql, query_tag(Q_ROWS, p->sel_server, p->sel_db, p->sel_table), &error)) { p->grid_loading = false; set_string(&p->grid_error, error); }
    free(d); free(t); free(sql); free(error);
}

/// The first column of every row after the header: the names SHOW DATABASES and SHOW TABLES answer with.
static char **first_column(const char *out, size_t *count) {
    SqlTable t; sql_table_parse(out, &t);
    char **names = t.rows > 1 ? xcalloc(t.rows - 1, sizeof *names) : NULL;
    *count = 0;
    for (size_t r = 1; r < t.rows; r++) names[(*count)++] = xstrdup(sql_cell(&t, r, 0));
    sql_table_free(&t);
    return names;
}
static void query_done(void *ctx, intptr_t tag, bool ok, const char *out) {
    ProjectDb *p = ctx;
    int kind = (int)(tag >> 56), s, d, t;
    unpack(tag & (((intptr_t)1 << 56) - 1), &s, &d, &t);
    ServerNode *n = server_at(p, s);
    if (!n) return;
    if (kind == Q_DATABASES) {
        n->loading = false; n->loaded = ok;
        set_string(&n->error, ok ? NULL : out);
        if (ok) {
            size_t count; char **names = first_column(out, &count);
            n->dbs = xcalloc(count ? count : 1, sizeof *n->dbs); n->db_count = count;
            for (size_t i = 0; i < count; i++) n->dbs[i].name = names[i];
            free(names);
        }
    } else if (kind == Q_TABLES) {
        DbNode *db = db_at(p, s, d);
        if (!db) return;
        db->loading = false; db->loaded = ok;
        set_string(&db->error, ok ? NULL : out);
        if (ok) db->tables = first_column(out, &db->table_count);
    } else {
        // Only the table still selected takes the answer.
        if (s != p->sel_server || d != p->sel_db || t != p->sel_table) return;
        grid_clear(p);
        if (ok) sql_table_parse(out, &p->grid);
        else set_string(&p->grid_error, out);
    }
    relayout(p);
}

// MARK: - Loading

static void rows_done(void *owner, Request *req) {
    ProjectDb *p = owner;
    p->loaded = true;
    if (!req->ok) request_error_into(&p->error, req);
    else {
        set_string(&p->error, NULL);
        servers_clear(p);
        const Json *all = json_get(req->result, "servers");
        for (size_t i = 0; i < json_count(all); i++) {
            const Json *row = json_at(all, i);
            if (!str_eq(json_str(json_get(row, "repo")), p->repo)) continue;
            p->servers = xrealloc(p->servers, (p->server_count + 1) * sizeof *p->servers);
            ServerNode *n = &p->servers[p->server_count++];
            memset(n, 0, sizeof *n);
            n->row = json_clone(row);
        }
    }
    relayout(p);
}
void project_db_load(ProjectDb *p) {
    if (p->loaded || p->req) return;
    if (!project_db_offered()) { p->loaded = true; return; }
    store_call("settings_ssh_servers", json_object(), 0, p, rows_done, 0, &p->req);
}
void project_db_refresh(ProjectDb *p) {
    request_cancel(&p->req);
    p->loaded = false;
    project_db_load(p);
}

static void login_done(void *owner, Request *req) {
    ProjectDb *p = owner;
    ServerNode *n = server_at(p, req->tag);
    if (!n) return;
    if (!req->ok) { n->loading = false; request_error_into(&n->error, req); relayout(p); return; }
    const Json *login = json_get(req->result, "credentials");
    if (!json_is_object(login)) login = req->result;
    json_free(n->login); n->login = json_clone(login);
    load_databases(p, req->tag);
    relayout(p);
}
/// Opens a server: its login read once, then its databases listed.
static void open_server(ProjectDb *p, int s) {
    ServerNode *n = server_at(p, s);
    n->open = !n->open;
    if (!n->open || n->loaded || n->loading) return;
    set_string(&n->error, NULL);
    if (!json_bool_is(json_get(n->row, "hasDbCredentials"), true)) {
        set_string(&n->error, "No database login is stored for this server. Add one to it under \xE2\x9A\x99 Settings \xE2\x86\x92 SSH servers.");
        return;
    }
    n->loading = true;
    if (n->login) { load_databases(p, s); return; }
    Json *args = json_object(); json_set_num(args, "id", server_id(n->row));
    store_call("ssh_db_credentials", args, 0, p, login_done, s, &n->req_login);
}
static void open_db(ProjectDb *p, int s, int d) {
    DbNode *db = db_at(p, s, d);
    db->open = !db->open;
    if (!db->open || db->loaded || db->loading) return;
    set_string(&db->error, NULL);
    load_tables(p, s, d);
}

// MARK: - The tree

typedef struct { char *name, *sub; int depth; wchar_t glyph; bool chevron, open, selected, dim; } NodeData;
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
        d->name = server_name(n->row);
        const char *user = json_str(json_get(n->row, "username")), *host = json_str(json_get(n->row, "host"));
        d->sub = xstrfmt("%s@%s:%d", user ? user : "", host ? host : "", server_port(n->row));
        d->glyph = 0xE1D3; d->chevron = true; d->open = n->open;
        d->dim = !json_bool_is(json_get(n->row, "hasDbCredentials"), true);
        add_node(p, doc, x, w, d, A_SERVER, pack((int)s, -1, -1));
        if (!n->open) continue;
        if (n->loading) node_note(doc, x, w, 1, "Connecting\xE2\x80\xA6", false);
        if (n->error) node_note(doc, x, w, 1, n->error, true);
        if (n->loaded && !n->db_count) node_note(doc, x, w, 1, "No databases.", false);
        for (size_t di = 0; di < n->db_count; di++) {
            DbNode *db = &n->dbs[di];
            NodeData *dd = xcalloc(1, sizeof *dd);
            dd->name = xstrdup(db->name); dd->depth = 1; dd->glyph = 0xE8B7; dd->chevron = true; dd->open = db->open;
            add_node(p, doc, x, w, dd, A_DB, pack((int)s, (int)di, -1));
            if (!db->open) continue;
            if (db->loading) node_note(doc, x, w, 2, "Loading tables\xE2\x80\xA6", false);
            if (db->error) node_note(doc, x, w, 2, db->error, true);
            if (db->loaded && !db->table_count) node_note(doc, x, w, 2, "No tables.", false);
            for (size_t ti = 0; ti < db->table_count; ti++) {
                NodeData *td = xcalloc(1, sizeof *td);
                td->name = xstrdup(db->tables[ti]); td->depth = 2; td->glyph = 0xE8FD;
                td->selected = (int)s == p->sel_server && (int)di == p->sel_db && (int)ti == p->sel_table;
                add_node(p, doc, x, w, td, A_TABLE, pack((int)s, (int)di, (int)ti));
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

typedef struct { ProjectDb *p; size_t row; } GridRowData;
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
    DbNode *db = db_at(p, p->sel_server, p->sel_db);
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
    if (room < px(240)) room = px(240);
    int lw = px(LIST_W);
    if (lw > w / 3) lw = w / 3;
    int tree_first = (int)doc->count;
    layout_tree(p, doc, 0, lw);
    // An unpainted spacer keeps the tree's window the view's height when the tree is shorter.
    RECT spacer = { lw + px(8), top, lw + px(9), top + room };
    doc_add(doc, &spacer, NULL);
    int tree_last = (int)doc->count;
    doc->y = top;
    int rx = lw + px(18), rw = w - rx;
    layout_grid(p, doc, rx, rw);
    if (doc->y < top + room) doc->y = top + room;
    // A rule between the tree and the grid, outside the tree's group so it does not scroll with the tree: it spans
    // the whole page and paints only the tree's window.
    RECT rule = { lw + px(8), top, lw + px(9), doc->y };
    doc_add(doc, &rule, paint_rule);
    doc_sticky(doc, tree_first, tree_last, doc->y);
}

// MARK: - Actions

bool project_db_action(ProjectDb *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_DB_ACTIONS) return false;
    int s, d, t;
    unpack(arg, &s, &d, &t);
    switch (action - p->base) {
    case A_SERVER: if (server_at(p, s)) open_server(p, s); break;
    case A_DB: if (db_at(p, s, d)) open_db(p, s, d); break;
    case A_TABLE:
        if (!db_at(p, s, d)) break;
        p->sel_server = s; p->sel_db = d; p->sel_table = t;
        load_rows(p);
        break;
    case A_RELOAD: load_rows(p); break;
    }
    relayout(p);
    return true;
}

// MARK: - Lifetime

ProjectDb *project_db_new(const char *repo, Screen *host, int action_base) {
    ProjectDb *p = xcalloc(1, sizeof *p);
    p->repo = xstrdup(repo ? repo : "");
    p->host = host; p->base = action_base;
    p->sel_server = p->sel_db = p->sel_table = -1;
    return p;
}
void project_db_free(ProjectDb *p) {
    if (!p) return;
    request_cancel(&p->req);
    servers_clear(p);
    free(p->repo); free(p->error);
    free(p);
}
