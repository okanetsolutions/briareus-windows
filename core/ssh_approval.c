#include "ssh_approval.h"
#include "str.h"
#include <stdlib.h>
#include <math.h>

static const char *sending = "Sending decision; do not submit it again.";
static const char *uncertain = "Outcome unknown. Check this request on the server; this decision will not be resubmitted.";

SshApprovalQueue *ssh_approval_queue(SshApprovalQueue **queues, const char *scope) {
    for (SshApprovalQueue *q = *queues; q; q = q->next) if (str_eq(q->scope, scope)) return q;
    SshApprovalQueue *q = xcalloc(1, sizeof *q); q->scope = xstrdup(scope); q->rows = json_object(); q->decisions = json_object();
    q->next = *queues; *queues = q; return q;
}
void ssh_approval_retain(SshApprovalQueue *q, const SshApprovalQueue *old) {
    if (q == old) return;
    q->retry_until = fmax(q->retry_until, old->retry_until);
    for (size_t i = 0; i < json_count(old->decisions); i++) {
        const char *id = json_key(old->decisions, i);
        const char *result = json_str(json_get(old->decisions, id));
        json_set_str(q->decisions, id, str_eq(result, sending) ? uncertain : result);
    }
}
void ssh_approval_free(SshApprovalQueue **queues) {
    while (*queues) { SshApprovalQueue *q = *queues; *queues = q->next; free(q->scope); free(q->error); json_free(q->rows); json_free(q->decisions); free(q); }
}
const Json *ssh_approval_row(const SshApprovalQueue *q, const char *id) { return json_get(q ? q->rows : NULL, id); }
static const char *text(const Json *r, const char *key) { return json_str_or(json_get(r, key), ""); }
bool ssh_approval_elevated(const Json *request) { return json_bool_is(json_get(request, "sudo"), true); }
const char *ssh_approval_action_label(const Json *r) {
    if (!json_is_null(json_get(r, "upload"))) return "Approve upload";
    return ssh_approval_elevated(r) ? "Run as root" : "Run command";
}
static bool pending(const Json *r, double now) {
    double expires = json_num_or(json_get(r, "expiresAt"), 0);
    return str_eq(text(r, "status"), "pending") && isfinite(expires) && expires > now * 1000;
}
static bool scoped(const Json *r, const Device *d) {
    if (!device_can_manage(d)) return false;
    if (!d->repo_count) return true;
    for (size_t i = 0; i < d->repo_count; i++) if (str_eq(d->repos[i], text(r, "repo"))) return true;
    return false;
}
bool ssh_approval_eligible(const SshApprovalQueue *q, const Json *r, const Device *d, double now) {
    if (!q || !q->fresh || q->retry_until > now || !scoped(r, d) || !pending(r, now)) return false;
    const Json *row = ssh_approval_row(q, text(r, "id"));
    return json_equal(json_get(row, "request"), r) && json_is_null(json_get(q->decisions, text(r, "id")));
}
Json *ssh_approval_body(const char *id, const char *decision) {
    if (!id || !*id || !(str_eq(decision, "approve") || str_eq(decision, "deny"))) return NULL;
    Json *body = json_object(); json_set_str(body, "id", id); json_set_str(body, "decision", decision); return body;
}
static Json *public_request(const Json *r) {
    if (!*text(r, "id") || !*text(r, "repo") || !*text(r, "jobId")) return NULL;
    if (!*text(r, "command") || !*text(r, "host") || !*text(r, "status")) return NULL;
    Json *out = json_object();
    const char *keys[] = {"id", "repo", "jobId", "sessionTitle", "serverId", "serverLabel", "host", "port", "username", "command", "status", "createdAt", "expiresAt", "timeoutSeconds", "sudo", "unattended"};
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) json_object_set(out, keys[i], json_clone(json_get(r, keys[i])));
    const Json *upload = json_get(r, "upload");
    if (!json_is_null(upload)) {
        Json *u = json_object(); const char *fields[] = {"sourcePath", "destination", "size", "sha256"};
        for (size_t i = 0; i < sizeof fields / sizeof *fields; i++) json_object_set(u, fields[i], json_clone(json_get(upload, fields[i])));
        json_object_set(out, "upload", u);
    }
    return out;
}
bool ssh_approval_read(SshApprovalQueue *q, const Json *answer, unsigned long long generation) {
    if (generation != q->generation) return false;
    const Json *requests = json_get(answer, "requests"); if (!json_is_array(requests)) return false;
    Json *rows = json_object();
    for (size_t i = 0; i < json_count(requests); i++) {
        Json *r = public_request(json_at(requests, i)); if (!r) continue;
        const char *id = text(r, "id"); const Json *result = json_get(q->decisions, id);
        Json *row = json_object(); json_object_set(row, "request", r);
        if (!json_is_null(result)) json_object_set(row, "result", json_clone(result));
        json_object_set(rows, id, row);
    }
    // Keep submitted/uncertain actions reviewable even when they leave the pending list.
    for (size_t i = 0; i < json_count(q->rows); i++) {
        const char *id = json_key(q->rows, i); const Json *old = json_get(q->rows, id);
        if (json_is_null(json_get(old, "result")) || !json_is_null(json_get(rows, id))) continue;
        Json *retained = json_clone(old);
        Json *r = (Json *)json_get(retained, "request");
        json_set_str(r, "status", "not in pending list");
        json_object_set(rows, id, retained);
    }
    json_free(q->rows); q->rows = rows; q->fresh = true; q->generation++; free(q->error); q->error = NULL; return true;
}
void ssh_approval_failure(SshApprovalQueue *q, const ApiError *error, double now) {
    q->fresh = false; q->generation++;
    double retry = error->retry_after;
    if (!isfinite(retry) || retry < 0) retry = error->status == 429 ? 60 : 7;
    q->retry_until = fmax(q->retry_until, now + retry);
    free(q->error); q->error = api_error_description(error);
    if (error->status == 401 || error->status == 403 || error->status == 404) {
        json_free(q->rows); q->rows = json_object();
    }
}
static void set_result(SshApprovalQueue *q, const char *id, Json *row, const char *message) {
    json_set_str(q->decisions, id, message);
    if (row->type == JSON_OBJECT) json_set_str(row, "result", message);
}
bool ssh_approval_begin(SshApprovalQueue *q, const Json *displayed, const Device *device, double now) {
    if (!ssh_approval_eligible(q, displayed, device, now)) return false;
    Json *row = (Json *)ssh_approval_row(q, text(displayed, "id"));
    set_result(q, text(displayed, "id"), row, sending); q->generation++; return true;
}
void ssh_approval_finish(SshApprovalQueue *q, const Json *sent, bool ok, const Json *answer, const ApiError *error, double now) {
    Json *row = (Json *)ssh_approval_row(q, text(sent, "id"));
    const Json *r = json_get(answer, "request"); const char *status = text(r, "status");
    bool matched = str_eq(text(r, "id"), text(sent, "id"));
    bool completed = str_eq(status, "running") || str_eq(status, "denied") || str_eq(status, "completed") || str_eq(status, "failed");
    q->generation++;
    if (ok && matched && completed) {
        Json *updated = json_clone(sent); json_set_str(updated, "status", status);
        if (row->type == JSON_OBJECT) json_object_set(row, "request", updated); else json_free(updated);
        set_result(q, text(sent, "id"), row, status); return;
    }
    // Even definite refusals require a fresh review; uncertain results are never retried.
    set_result(q, text(sent, "id"), row, !ok && api_error_is_refusal(error)
        ? "Decision refused. Refresh to check current state; review on the server before taking further action."
        : uncertain);
    // A decision's 404 means this item vanished, rather than loss of the entire list.
    ApiError copy = *error; if (copy.status == 404) copy.status = 400;
    ssh_approval_failure(q, &copy, now);
}
size_t ssh_approval_waiting(const SshApprovalQueue *q, double now) {
    size_t count = 0; if (!q || !q->fresh) return 0;
    for (size_t i = 0; i < json_count(q->rows); i++) if (pending(json_get(json_get(q->rows, json_key(q->rows, i)), "request"), now)) count++;
    return count;
}
char *ssh_approval_detail(const Json *r, double now) {
    Str s; str_init(&s);
    str_appendf(&s, "SSH approval%s\nServer: %s (ID %d)\n%s@%s:%d\nProject: %s\nSession: %s (%s)\nRequest: %s\nStatus: %s\nExpires: %.0f (Unix milliseconds)\nTimeout: %d seconds\n",
        ssh_approval_elevated(r) ? " as root" : "", text(r, "serverLabel"), json_int_or(json_get(r, "serverId"), 0),
        text(r, "username"), text(r, "host"), json_int_or(json_get(r, "port"), 22), text(r, "repo"), text(r, "sessionTitle"),
        text(r, "jobId"), text(r, "id"), pending(r, now) ? "pending" : str_eq(text(r, "status"), "pending") ? "expired" : text(r, "status"),
        json_num_or(json_get(r, "expiresAt"), 0), json_int_or(json_get(r, "timeoutSeconds"), 0));
    // The contract does not report permissionMode on a pending request; do not infer ask/allow.
    str_appendz(&s, json_bool_is(json_get(r, "unattended"), true) ? "Asked in an unattended turn; operator approval required.\n" : "Operator approval required.\n");
    str_appendz(&s, "Server permission mode is not reported by the approval API.\n");
    const Json *u = json_get(r, "upload");
    if (!json_is_null(u)) str_appendf(&s, "Upload: %s\nDestination: %s\nSize: %.0f bytes\nSHA256: %s\nAtomic replacement, mode 0600\n", text(u, "sourcePath"), text(u, "destination"), json_num_or(json_get(u, "size"), 0), text(u, "sha256"));
    str_appendf(&s, "\n%s", text(r, "command")); return str_detach(&s);
}
