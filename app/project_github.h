#ifndef BRIAREUS_PROJECT_GITHUB_H
#define BRIAREUS_PROJECT_GITHUB_H
#include "screens.h"

/// Owned project diagnostics; requests are cancelled before releasing the enclosing screen.
typedef struct {
    Screen *screen;
    char *repo, *device, *permission;
    ApiClient *client;
    Request *identity_request, *health_request;
    Json *account, *health;
    char *identity_error, *health_error;
    bool loaded;
} ProjectGithub;

enum { ACT_PROJECT_GITHUB_CHECK = 1190, ACT_PROJECT_GITHUB_RELOAD };
void project_github_init(ProjectGithub *g, Screen *screen, const char *repo);
void project_github_clear(ProjectGithub *g);
void project_github_load(ProjectGithub *g, bool retry);
void project_github_check(ProjectGithub *g);
void project_github_layout(ProjectGithub *g, Doc *doc, int x, int w);
bool project_github_offered(void);
#endif
