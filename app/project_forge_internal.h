#ifndef BRIAREUS_PROJECT_FORGE_INTERNAL_H
#define BRIAREUS_PROJECT_FORGE_INTERNAL_H
#include "screens.h"
#include "str.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A project's Forge tab, beside its pull requests and issues: the servers of the Laravel Forge accounts the project may
// use (Settings → Forge accounts) down the left, and the server picked there on the right with its Forge sites, each with
// its domain, state, repository and branch, deployment and PHP. The server reads Forge with the account's token
// (/forge/accounts/{account}/servers and …/sites), so the token never reaches this PC; both need an Admin token.

// The actions, from the host's `action_base` up.
// The site open in the tab takes the next FORGE_SITE_ACTIONS, from A_SITE_VIEW up.
enum { A_SERVER, A_OPEN_SITE, A_SITE_PAGE, A_BACK, A_SITE_VIEW = 8 };

enum { LIST_W = 280, MAX_PAGES = 20 };

struct ProjectForge {
    char *repo;
    Screen *host;
    int base;
    Json *accounts;        // the ForgeAccount rows available to the project
    Json *servers;         // every account's ForgeServer rows, each with `_account` and `_accountLabel` added
    size_t walking;        // the account whose servers are being read
    int pages;             // pages of it read so far
    bool loaded;
    char *error;           // the accounts' or a server list's
    Request *req;
    char *selected;        // the server on show, as "account:server"
    Json *sites;           // "account:server" → its ForgeSite rows, once read in full
    Json *incoming;        // the pages of the server being read
    int site_pages;
    char *sites_error;
    Request *req_sites;
    ForgeSite *site;       // the site open in place of the server's list, if any
};

bool project_forge_offered(void);
char *project_forge_server_key(const Json *server);
const Json *project_forge_selected_server(ProjectForge *p);
void project_forge_load(ProjectForge *p);
void project_forge_refresh(ProjectForge *p);
char *project_forge_server_detail(const Json *server);
void project_forge_layout(ProjectForge *p, Doc *doc, int w);
void project_forge_header(ProjectForge *p, HeaderInfo *info);
bool project_forge_action(ProjectForge *p, int action, intptr_t arg, POINT pt);
void project_forge_place(ProjectForge *p, const RECT *content, int scroll_y, bool shown);
bool project_forge_command(ProjectForge *p, int id, int code);
bool project_forge_key(ProjectForge *p, WPARAM vk, bool ctrl);
bool project_forge_can_leave(ProjectForge *p);
ProjectForge *project_forge_new(const char *repo, Screen *host, int action_base);
void project_forge_free(ProjectForge *p);

#endif
