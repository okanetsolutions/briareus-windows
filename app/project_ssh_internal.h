#ifndef BRIAREUS_PROJECT_SSH_INTERNAL_H
#define BRIAREUS_PROJECT_SSH_INTERNAL_H
#include "screens.h"
#include "terminal.h"

// The actions, from the host's `action_base` up.
enum { A_SERVER, A_TAB, A_TAB_CLOSE, A_RECONNECT, A_CLOSE };
enum { LIST_W = 260, TAB_H = 30, TAB_CLOSE_W = 22, MAX_INSTANCES = 16 };

struct ProjectSsh {
    char *repo;
    Screen *host;
    int base;
    Json *rows;            // the project's SshServer rows
    bool loaded, loading;
    char *error;
    Request *req;
    RECT term_rc;          // the terminal's area, in content coordinates
    Term *focused;         // the session last given the keyboard
};

char *project_ssh_server_key(const Json *row);
char *project_ssh_server_name(const Json *row);
int project_ssh_server_port(const Json *row);
Term *project_ssh_shown_term(ProjectSsh *p);

#endif
