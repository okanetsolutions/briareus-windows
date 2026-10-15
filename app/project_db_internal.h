#ifndef BRIAREUS_PROJECT_DB_INTERNAL_H
#define BRIAREUS_PROJECT_DB_INTERNAL_H
#include "canvas.h"
#include "screens.h"
#include "ssh_query.h"
#include "str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A project's Database tab, beside its SSH and SFTP sessions, laid out as a database browser: the project's SSH servers
// down the left as a tree (a server opens to its databases, a database to its tables), and the table clicked on the right
// as a grid of its first rows. Nothing goes through the Briareus server but the database login stored with the SSH server
// (GET …/db-credentials): every query is the server's own `mysql` client run over SSH from this PC (ssh_query.c).

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

bool project_db_offered(void);
intptr_t project_db_pack(int s, int d, int t);
char *project_db_server_name(const Json *row);
int project_db_server_port(const Json *row);
DbNode *project_db_db_at(ProjectDb *p, int s, int d);
void project_db_load(ProjectDb *p);
void project_db_refresh(ProjectDb *p);
void project_db_layout(ProjectDb *p, Doc *doc, int w);
bool project_db_action(ProjectDb *p, int action, intptr_t arg, POINT pt);
ProjectDb *project_db_new(const char *repo, Screen *host, int action_base);
void project_db_free(ProjectDb *p);

#endif
