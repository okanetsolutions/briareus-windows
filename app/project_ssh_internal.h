#ifndef BRIAREUS_PROJECT_SSH_INTERNAL_H
#define BRIAREUS_PROJECT_SSH_INTERNAL_H
#include "screens.h"
#include "terminal.h"
#include "ssh_servers.h"

// The actions, from the host's `action_base` up.
enum { A_SERVER, A_TAB, A_TAB_CLOSE, A_RECONNECT, A_CLOSE, A_FOLDER, A_CLEAR };
enum { LIST_W = 260, TAB_H = 30, TAB_CLOSE_W = 22, MAX_INSTANCES = 16 };

struct ProjectSsh {
    char *repo;
    bool global;
    ApiClient *account;
    char *scope, *query;
    Json *folded;
    Screen *host;
    int base;
    Json *rows;            // the project's SshServer rows
    bool loaded, loading;
    char *error;
    Request *req;
    RECT term_rc;          // the terminal's area, in content coordinates
    Term *focused;         // the session last given the keyboard
};

int project_ssh_server_port(const Json *row);
Term *project_ssh_shown_term(ProjectSsh *p);

char *project_ssh_scope(void);
bool project_ssh_current(ProjectSsh *p);
void servers_ssh_sessions_bind(void);
void servers_ssh_sessions_check(void);
void project_ssh_connect_row(ProjectSsh *p, const Json *row);
void project_ssh_layout_server(ProjectSsh *p, Doc *doc, int x, int w, size_t i);
char *project_ssh_tab_label(ProjectSsh *p, Term *t);
void project_ssh_global_list(ProjectSsh *p, Doc *doc, int w);
#endif
