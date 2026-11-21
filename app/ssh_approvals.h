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
    void (*changed)(Screen *owner);   // optional: called whenever the shared queue changes, for counts kept elsewhere
    struct SshApprovals *next;
} SshApprovals;
enum { SSH_APPROVE = 1900, SSH_DENY, SSH_REFRESH, SSH_POLL };
SshApprovals *ssh_approvals_new(Screen *owner);
void ssh_approvals_free(SshApprovals *s);
void ssh_approvals_visible(SshApprovals *s, bool shown);
/// Lets `s` read without a poller of its own: its owner calls ssh_approvals_refresh on its own schedule.
void ssh_approvals_watch(SshApprovals *s, bool shown);
/// The first read is under way: nothing read yet, and no failure to show.
bool ssh_approvals_loading(SshApprovals *s);
void ssh_approvals_refresh(SshApprovals *s);
void ssh_approvals_timer(SshApprovals *s, UINT id);
void ssh_approvals_layout(SshApprovals *s, Doc *doc, int x, int w);
void ssh_approvals_action(SshApprovals *s, int action, intptr_t token);
/// Whether this device may list SSH approvals on this server: a Manage token and the `ssh_requests` route.
bool ssh_approvals_offered(void);
/// True once this device's queue holds a successful read, with `*waiting` how many wait; false (and 0) before that,
/// after a failed read, or when not offered.
bool ssh_approvals_counted(size_t *waiting);
size_t ssh_approvals_badge(void);
#endif
