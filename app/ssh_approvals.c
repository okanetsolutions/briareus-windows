#include "ssh_approvals.h"
#include "str.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>

static SshApprovals *views;
static bool current(SshApprovals *s) {
    if (s->client != g_store.client || !store_can_manage() || !store_supports("ssh_requests")) return false;
    Json *d = device_json(&g_store.device); bool same = json_equal(d, s->device); json_free(d); return same;
}
static SshApprovalQueue *queue(SshApprovals *s) { return ssh_approval_queue(&g_store.ssh_approvals, s->scope); }
static void retain_decisions(SshApprovalQueue *q) {
    for (SshApprovalQueue *old = g_store.ssh_approvals; old; old = old->next) {
        if (old == q) continue;
        for (size_t i = 0; i < json_count(old->decisions); i++) {
            const char *id = json_key(old->decisions, i);
            json_object_set(q->decisions, id, json_clone(json_get(old->decisions, id)));
        }
    }
}
// Rebinding discards displayed metadata, but submitted decisions survive until disconnect.
static void rebind(SshApprovals *s) {
    request_cancel(&s->read); request_cancel(&s->write);
    json_free(s->bindings); s->bindings = NULL;
    json_free(s->device); s->device = device_json(&g_store.device);
    free(s->scope); s->scope = json_serialize(s->device, true);
    SshApprovalQueue *q = queue(s);
    json_free(q->rows); q->rows = json_object(); q->fresh = false; q->generation++;
    retain_decisions(q);
}
static void notify(void) {
    for (SshApprovals *s = views; s; s = s->next) {
        if (!s->owner->pane) continue;
        pane_relayout(s->owner->pane); pane_header_changed(s->owner->pane);
    }
    projects_recount_findings();
}
// Finalizers own only scope and input metadata; they never borrow the screen or queue.
typedef struct { Json *device, *sent; char *scope; unsigned long long generation; } Completion;
static void completed(void *ctx, Request *req) {
    Completion *c = ctx; req->auth_checked = true;
    Json *d = device_json(&g_store.device);
    bool same = req->client == g_store.client && g_store.has_device && json_equal(d, c->device);
    json_free(d);
    if (same && store_can_manage() && store_supports("ssh_requests")) {
        SshApprovalQueue *q = ssh_approval_queue(&g_store.ssh_approvals, c->scope);
        if (c->sent) ssh_approval_finish(q, c->sent, req->ok, req->result, &req->error, (double)time(NULL));
        else if (!req->ok) ssh_approval_failure(q, &req->error, (double)time(NULL));
        else if (c->generation == q->generation && !ssh_approval_read(q, req->result, c->generation)) {
            ApiError error; api_error_init(&error); api_error_set(&error, API_NON_JSON, 0, "Unexpected SSH requests response", -1);
            ssh_approval_failure(q, &error, (double)time(NULL)); api_error_clear(&error);
        }
        if (!req->ok && api_error_unauthorized(&req->error)) store_invalidate_credentials(&req->error);
        notify();
    }
    json_free(c->device); json_free(c->sent); free(c->scope); free(c); req->completion_ctx = NULL;
}
static void attach(SshApprovals *s, Request *req, const Json *sent) {
    Completion *c = xcalloc(1, sizeof *c); c->device = json_clone(s->device); c->scope = xstrdup(s->scope);
    c->sent = sent ? json_clone(sent) : NULL; c->generation = queue(s)->generation;
    req->completion_ctx = c; req->completed = completed;
}
void ssh_approvals_refresh(SshApprovals *s) {
    if (!s || !s->shown || !g_store.active || s->client != g_store.client) return;
    if (!store_can_manage() || !store_supports("ssh_requests")) return;
    if (!current(s)) rebind(s);
    if (s->read) return;
    if (queue(s)->retry_until > (double)time(NULL)) return;
    Request *req = store_call("ssh_requests", json_object(), 0, NULL, NULL, 0, &s->read); attach(s, req, NULL);
}
void ssh_approvals_timer(SshApprovals *s, UINT id) {
    if (!s || !poller_fired(&s->poller, id)) return;
    ssh_approvals_refresh(s);
    poller_finished(&s->poller, false, -1);
}
void ssh_approvals_visible(SshApprovals *s, bool shown) {
    if (!s) return;
    s->shown = shown;
    if (shown) { if (s->owner->pane) poller_start(&s->poller, s->owner->pane, SSH_POLL, 7000); ssh_approvals_refresh(s); }
    else { poller_stop(&s->poller); request_cancel(&s->read); json_free(s->bindings); s->bindings = NULL; }
}
SshApprovals *ssh_approvals_new(Screen *owner) {
    SshApprovals *s = xcalloc(1, sizeof *s); s->owner = owner;
    s->client = api_client_retain(g_store.client); s->device = device_json(&g_store.device);
    s->scope = json_serialize(s->device, true); retain_decisions(queue(s)); s->next = views; views = s; return s;
}
void ssh_approvals_free(SshApprovals *s) {
    if (!s) return;
    poller_stop(&s->poller);
    request_cancel(&s->read); request_cancel(&s->write);
    SshApprovals **slot = &views; while (*slot && *slot != s) slot = &(*slot)->next;
    if (*slot) *slot = s->next;
    json_free(s->device); json_free(s->bindings); free(s->scope); api_client_release(s->client); free(s);
}
size_t ssh_approvals_badge(void) {
    if (!store_can_manage() || !store_supports("ssh_requests")) return 0;
    Json *d = device_json(&g_store.device); char *scope = json_serialize(d, true); json_free(d);
    SshApprovalQueue *q = ssh_approval_queue(&g_store.ssh_approvals, scope); free(scope);
    return ssh_approval_waiting(q, (double)time(NULL));
}
void ssh_approvals_action(SshApprovals *s, int action, intptr_t token) {
    if (action == SSH_REFRESH) { ssh_approvals_refresh(s); notify(); return; }
    if (!s || !s->shown || !current(s)) return;
    if (s->read || s->write || !store_supports("ssh_decision")) return;
    SshApprovalQueue *q = queue(s); if (q->generation != s->rendered_generation) return;
    char key[32]; snprintf(key, sizeof key, "%lld", (long long)token);
    const Json *displayed = json_get(s->bindings, key);
    const char *id = json_str(json_get(displayed, "id"));
    Json *body = ssh_approval_body(id, action == SSH_APPROVE ? "approve" : action == SSH_DENY ? "deny" : NULL);
    if (!body) return;
    if (!ssh_approval_begin(q, displayed, &g_store.device, (double)time(NULL))) { json_free(body); return; }
    Request *req = store_call("ssh_decision", body, 0, NULL, NULL, 0, &s->write); attach(s, req, displayed);
    notify();
}
void ssh_approvals_layout(SshApprovals *s, Doc *doc, int x, int w) {
    if (!s) return;
    if (!current(s)) ssh_approvals_refresh(s);
    json_free(s->bindings); s->bindings = json_object();
    if (!current(s)) return;
    SshApprovalQueue *q = queue(s); s->rendered_generation = q->generation;
    doc_text(doc, x, w, "Waiting on you: SSH approvals", FONT_BODY_SEMIBOLD, theme.ink, DT_WORDBREAK);
    ButtonSpec refresh = {0, "Refresh SSH approvals", BUTTON_PLAIN, SSH_REFRESH, 0, !s->read};
    doc_button_row(doc, x, w, &refresh, 1);
    if (q->error) doc_notice(doc, x, w, q->error);
    if (q->retry_until > (double)time(NULL)) doc_text(doc, x, w, "Server cooldown active. Refresh will resume after it expires.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    if (!q->fresh) doc_text(doc, x, w, "Refresh SSH approvals before deciding.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    for (size_t i = 0; i < json_count(q->rows); i++) {
        const Json *row = json_get(q->rows, json_key(q->rows, i)), *r = json_get(row, "request");
        int box = doc_box_begin(doc, x, w, px(10), theme.raise, theme.line, px(8));
        doc_item(doc, box)->hover_fill = false;
        double expires = json_num_or(json_get(r, "expiresAt"), 0);
        if (expires > 0 && expires < 4102444800001.0) {
            char *when = format_event_time((time_t)(expires / 1000));
            char *line = xstrfmt("Expires %s", when);
            doc_text(doc, x + px(12), w - px(24), line, FONT_CAPTION, theme.muted, DT_WORDBREAK); free(line); free(when);
        }
        char *detail = ssh_approval_detail(r, (double)time(NULL));
        doc_text(doc, x + px(12), w - px(24), detail, FONT_MONO_CAPTION2, theme.ink, DT_WORDBREAK); free(detail);
        const char *result = json_str(json_get(row, "result"));
        if (result) doc_text(doc, x + px(12), w - px(24), result, FONT_CAPTION, theme.secondary, DT_WORDBREAK);
        bool eligible = ssh_approval_eligible(q, r, &g_store.device, (double)time(NULL));
        if (eligible && store_supports("ssh_decision") && s->serial < INTPTR_MAX) {
            intptr_t token = ++s->serial; char key[32]; snprintf(key, sizeof key, "%lld", (long long)token);
            json_object_set(s->bindings, key, json_clone(r));
            ButtonSpec buttons[] = {{0, ssh_approval_action_label(r), BUTTON_BORDERED, SSH_APPROVE, token, !s->read && !s->write},
                {0, "Deny", BUTTON_DESTRUCTIVE, SSH_DENY, token, !s->read && !s->write}};
            doc_button_row(doc, x + px(12), w - px(24), buttons, 2);
        }
        doc_box_end(doc, box, px(10)); doc_space(doc, px(10));
    }
    if (q->fresh && !json_count(q->rows)) doc_text(doc, x, w, "No SSH approval is waiting.", FONT_CAPTION, theme.muted, DT_WORDBREAK);
    doc_space(doc, px(12));
}
