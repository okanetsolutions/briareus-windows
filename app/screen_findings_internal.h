#ifndef BRIAREUS_SCREEN_FINDINGS_INTERNAL_H
#define BRIAREUS_SCREEN_FINDINGS_INTERNAL_H
#include "screens.h"
#include "ssh_approvals.h"
#include <stdio.h>

// The Findings screen: the SSH commands waiting for this device's decision. The review rounds it also queued were
// retired by the server (core #228); a pull request's stored code reviews are read on its Code reviews tab.

typedef struct {
    Screen base;
    SshApprovals *ssh;
} FindingsScreen;

Screen *findings_screen_new(void);

#endif
