#ifndef BRIAREUS_SSH_APPROVALS_H
#define BRIAREUS_SSH_APPROVALS_H
#include "screens.h"
typedef struct SshApprovals {
    Screen *owner; // borrowed, requests cancelled before owner destruction
    ApiClient *client;
    Json *device, *bindings;
    char *scope;
    Request *read, *write;
    bool shown;
    Poller poller;
    intptr_t serial;
    unsigned long long rendered_generation;
    struct SshApprovals *next;
} SshApprovals;
enum { SSH_APPROVE = 1900, SSH_DENY, SSH_REFRESH, SSH_POLL };
SshApprovals *ssh_approvals_new(Screen *owner);
void ssh_approvals_free(SshApprovals *s);
void ssh_approvals_visible(SshApprovals *s, bool shown);
void ssh_approvals_refresh(SshApprovals *s);
void ssh_approvals_timer(SshApprovals *s, UINT id);
void ssh_approvals_layout(SshApprovals *s, Doc *doc, int x, int w);
void ssh_approvals_action(SshApprovals *s, int action, intptr_t token);
size_t ssh_approvals_badge(void);
#endif
