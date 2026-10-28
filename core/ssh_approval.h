#ifndef BRIAREUS_SSH_APPROVAL_H
#define BRIAREUS_SSH_APPROVAL_H
#include "api.h"

// Memory-only, connection-owned queues partitioned by the complete device/project scope.
// Rows retain only public review metadata. Unknown write outcomes remain interlocked until disconnect.
typedef struct SshApprovalQueue {
    char *scope, *error;
    Json *rows, *decisions; // decisions are ID-only tombstones, retained across metadata refusals
    bool fresh;
    double retry_until;
    unsigned long long generation;
    struct SshApprovalQueue *next;
} SshApprovalQueue;
SshApprovalQueue *ssh_approval_queue(SshApprovalQueue **queues, const char *scope);
// Retain connection interlocks and cooldowns, treating transferred in-flight decisions as uncertain.
void ssh_approval_retain(SshApprovalQueue *q, const SshApprovalQueue *old);
void ssh_approval_free(SshApprovalQueue **queues);
// Returned row is borrowed; detail/body are owned. No secret or file content fields are copied.
const Json *ssh_approval_row(const SshApprovalQueue *q, const char *id);
const char *ssh_approval_action_label(const Json *request);
char *ssh_approval_detail(const Json *request, double now);
bool ssh_approval_elevated(const Json *request);
bool ssh_approval_eligible(const SshApprovalQueue *q, const Json *request, const Device *device, double now);
Json *ssh_approval_body(const char *id, const char *decision);
bool ssh_approval_read(SshApprovalQueue *q, const Json *answer, unsigned long long generation);
void ssh_approval_failure(SshApprovalQueue *q, const ApiError *error, double now);
bool ssh_approval_begin(SshApprovalQueue *q, const Json *displayed, const Device *device, double now);
void ssh_approval_finish(SshApprovalQueue *q, const Json *sent, bool ok, const Json *answer, const ApiError *error, double now);
size_t ssh_approval_waiting(const SshApprovalQueue *q, double now);
#endif
