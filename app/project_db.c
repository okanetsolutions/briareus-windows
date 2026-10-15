#include "project_db_internal.h"

static void unpack(intptr_t v, int *s, int *d, int *t);
static intptr_t query_tag(int kind, int s, int d, int t);
static double server_id(const Json *row);
static ServerNode *server_at(ProjectDb *p, int s);
static void relayout(ProjectDb *p);
static void grid_clear(ProjectDb *p);
static void db_free(DbNode *d);
static void server_free(ServerNode *n);
static void servers_clear(ProjectDb *p);
static bool run(ProjectDb *p, int s, const char *sql, intptr_t tag, char **error);
static void load_databases(ProjectDb *p, int s);
static void load_tables(ProjectDb *p, int s, int d);
static void load_rows(ProjectDb *p);
static char **first_column(const char *out, size_t *count);
static void query_done(void *ctx, intptr_t tag, bool ok, const char *out);
static void rows_done(void *owner, Request *req);
static void login_done(void *owner, Request *req);
static void open_server(ProjectDb *p, int s);
static void open_db(ProjectDb *p, int s, int d);

bool project_db_offered(void) { return store_supports("settings_ssh_servers") && store_supports("ssh_db_credentials"); }

// A tree position packed into an action's argument, or a query's tag (with its kind on top).
intptr_t project_db_pack(int s, int d, int t) { return ((intptr_t)s << 40) | ((intptr_t)(d + 1) << 20) | (intptr_t)(t + 1); }

static void unpack(intptr_t v, int *s, int *d, int *t) {
    *s = (int)((v >> 40) & 0xFFFF); *d = (int)((v >> 20) & 0xFFFFF) - 1; *t = (int)(v & 0xFFFFF) - 1;
}

static intptr_t query_tag(int kind, int s, int d, int t) { return ((intptr_t)kind << 56) | project_db_pack(s, d, t); }

char *project_db_server_name(const Json *row) {
    const char *label = json_str_nonempty(json_get(row, "label"));
    if (label) return xstrdup(label);
    const char *user = json_str(json_get(row, "username")), *host = json_str(json_get(row, "host"));
    return user && *user ? xstrfmt("%s@%s", user, host ? host : "") : xstrdup(host ? host : "SSH server");
}

/// Its id, which is a timestamp-sized number, past an int.
static double server_id(const Json *row) { double id = 0; json_num(json_get(row, "id"), &id); return id; }

int project_db_server_port(const Json *row) { int port = json_int_or(json_get(row, "port"), 22); return port > 0 ? port : 22; }

static ServerNode *server_at(ProjectDb *p, int s) { return s >= 0 && (size_t)s < p->server_count ? &p->servers[s] : NULL; }

DbNode *project_db_db_at(ProjectDb *p, int s, int d) { ServerNode *n = server_at(p, s); return n && d >= 0 && (size_t)d < n->db_count ? &n->dbs[d] : NULL; }

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

/// Runs `sql` on server `s` with its database login, the answer coming back to query_done under `tag`.
static bool run(ProjectDb *p, int s, const char *sql, intptr_t tag, char **error) {
    ServerNode *n = server_at(p, s);
    const Json *row = n->row, *login = n->login;
    char *key = xstrfmt("db:%.0f", server_id(row)), *name = project_db_server_name(row);
    TermTarget target = { key, p->repo, name, json_str(json_get(row, "username")), json_str(json_get(row, "host")), project_db_server_port(row) };
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
    DbNode *db = project_db_db_at(p, s, d);
    char *ident = sql_ident(db->name), *sql = xstrfmt("SHOW TABLES FROM %s;", ident), *error = NULL;
    db->loading = true;
    if (!run(p, s, sql, query_tag(Q_TABLES, s, d, -1), &error)) { db->loading = false; set_string(&db->error, error); }
    free(ident); free(sql); free(error);
}

static void load_rows(ProjectDb *p) {
    DbNode *db = project_db_db_at(p, p->sel_server, p->sel_db);
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
        DbNode *db = project_db_db_at(p, s, d);
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
    DbNode *db = project_db_db_at(p, s, d);
    db->open = !db->open;
    if (!db->open || db->loaded || db->loading) return;
    set_string(&db->error, NULL);
    load_tables(p, s, d);
}

// MARK: - Actions

bool project_db_action(ProjectDb *p, int action, intptr_t arg, POINT pt) {
    (void)pt;
    if (action < p->base || action >= p->base + PROJECT_DB_ACTIONS) return false;
    int s, d, t;
    unpack(arg, &s, &d, &t);
    switch (action - p->base) {
    case A_SERVER: if (server_at(p, s)) open_server(p, s); break;
    case A_DB: if (project_db_db_at(p, s, d)) open_db(p, s, d); break;
    case A_TABLE:
        if (!project_db_db_at(p, s, d)) break;
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
