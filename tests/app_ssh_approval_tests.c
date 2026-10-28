#include "screen_fixture.h"
#include "ssh_approvals.h"
#include "screen_findings_internal.h"
#include "suites.h"
#include "fixtures/ssh_approval.h"

enum { STEPS = 12 };
typedef struct { HANDLE entered, release; const char *answer, *retry; int status; bool fail; char *method, *url; Json *body; } Step;
typedef struct { ScreenFixture screen; Screen owner; Step steps[STEPS]; volatile LONG calls; } Fixture;
static Route ROUTES[] = {{"GET", "/ssh/requests", "manage"}, {"POST", "/ssh/requests/{id}/decision", "manage"}};
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; Fixture *f = ctx; LONG i = InterlockedIncrement(&f->calls) - 1;
    CHECK(i < STEPS); if (i >= STEPS) return false;
    // Metadata is read only after this step's entered event. Tests release one deterministic request at a time.
    Step *s = &f->steps[i];
    s->method = xstrdup(method); s->url = xstrdup(url); char *text = xstrndup(body ? body : "", bytes);
    s->body = json_parsez(text); free(text); SetEvent(s->entered); CHECK_INT(WaitForSingleObject(s->release, 10000), WAIT_OBJECT_0);
    if (s->fail) { *error = xstrdup("Fixture timeout"); return false; }
    *status = s->status; *type = xstrdup("application/json"); *retry = xstrdup(s->retry);
    *response = xstrdup(s->answer); *len = strlen(*response); return true;
}
static void init(Fixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, ROUTES, 2);
    for (int i = 0; i < STEPS; i++) f->steps[i] = (Step){CreateEventW(NULL, TRUE, FALSE, NULL), CreateEventW(NULL, TRUE, FALSE, NULL), SSH_REQUESTS, NULL, 200, false};
    api_client_set_transport(f->screen.client, transport, f);
}
static void entered(Fixture *f, int i) { CHECK_INT(WaitForSingleObject(f->steps[i].entered, 10000), WAIT_OBJECT_0); }
static void complete(Fixture *f, int i) { entered(f, i); SetEvent(f->steps[i].release); screen_fixture_pump(&f->screen, 1); }
static void clear(Fixture *f) {
    ssh_approval_free(&g_store.ssh_approvals);
    for (int i = 0; i < STEPS; i++) { CloseHandle(f->steps[i].entered); CloseHandle(f->steps[i].release); free(f->steps[i].method); free(f->steps[i].url); json_free(f->steps[i].body); }
    screen_fixture_clear(&f->screen);
}
static SshApprovalQueue *queue(SshApprovals *s) { return ssh_approval_queue(&g_store.ssh_approvals, s->scope); }
static intptr_t draw(SshApprovals *s) {
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 800); ssh_approvals_layout(s, &doc, 0, 800); doc_end(&doc); doc_free(&doc);
    const char *key = json_key(s->bindings, 0); return key ? (intptr_t)strtoll(key, NULL, 10) : 0;
}
static void gating_and_exact_wire(void) {
    Fixture f; init(&f); g_store.device.permission = "read";
    SshApprovals *s = ssh_approvals_new(&f.owner); ssh_approvals_visible(s, true); CHECK(s->read == NULL); CHECK_INT(draw(s), 0);
    ssh_approvals_action(s, SSH_APPROVE, 1); CHECK_INT(f.calls, 0); ssh_approvals_free(s);
    g_store.device.permission = "manage"; s = ssh_approvals_new(&f.owner);
    g_store.route_count = 0; ssh_approvals_visible(s, true); CHECK(s->read == NULL); g_store.route_count = 2;
    ssh_approvals_refresh(s); entered(&f, 0); CHECK_STR(f.steps[0].method, "GET"); CHECK_STR(f.steps[0].url, "https://example.com/api/v1/ssh/requests"); CHECK(f.steps[0].body == NULL); complete(&f, 0);
    CHECK_INT(ssh_approvals_badge(), 1); intptr_t token = draw(s); CHECK(token != 0);
    g_store.route_count = 1; ssh_approvals_action(s, SSH_APPROVE, token); CHECK(s->write == NULL); g_store.route_count = 2;
    g_store.device.permission = "read"; ssh_approvals_action(s, SSH_APPROVE, token); CHECK(s->write == NULL); g_store.device.permission = "manage";
    // A subsequent read invalidates the exact prior drawing, even when the payload is identical.
    ssh_approvals_refresh(s); complete(&f, 1); ssh_approvals_action(s, SSH_APPROVE, token); CHECK(s->write == NULL);
    token = draw(s); f.steps[2].answer = "{\"request\":{\"id\":\"fake-request\",\"status\":\"running\"}}";
    ssh_approvals_action(s, SSH_APPROVE, token); entered(&f, 2);
    CHECK_STR(f.steps[2].method, "POST"); CHECK_STR(f.steps[2].url, "https://example.com/api/v1/ssh/requests/fake-request/decision");
    CHECK_INT(json_count(f.steps[2].body), 1); CHECK_STR(json_str(json_get(f.steps[2].body, "decision")), "approve");
    ssh_approvals_action(s, SSH_APPROVE, token); CHECK_INT(f.calls, 3); complete(&f, 2);
    CHECK_INT(ssh_approvals_badge(), 0); CHECK_INT(draw(s), 0); ssh_approvals_free(s); clear(&f);
}
static void multiple_views_uncertain_and_teardown(void) {
    Fixture f; init(&f); SshApprovals *a = ssh_approvals_new(&f.owner), *b = ssh_approvals_new(&f.owner);
    ssh_approvals_visible(a, true); complete(&f, 0);
    ssh_approvals_visible(b, true); complete(&f, 1);
    intptr_t ta = draw(a), tb = draw(b); CHECK(ta && tb);
    f.steps[2].fail = true; ssh_approvals_action(a, SSH_DENY, ta); entered(&f, 2);
    CHECK_STR(json_str(json_get(f.steps[2].body, "decision")), "deny"); CHECK_INT(json_count(f.steps[2].body), 1);
    ssh_approvals_action(b, SSH_APPROVE, tb); CHECK(b->write == NULL);
    ssh_approvals_free(a); complete(&f, 2); // No UI owner remains for the write finalizer.
    CHECK(!queue(b)->fresh); CHECK(queue(b)->error != NULL); CHECK_INT(draw(b), 0);
    queue(b)->retry_until = 0; ssh_approvals_refresh(b); complete(&f, 3);
    CHECK(queue(b)->fresh); CHECK_INT(draw(b), 0); // Pending on the server never silently unlocks an uncertain decision.
    ssh_approvals_free(b); b = ssh_approvals_new(&f.owner); ssh_approvals_visible(b, true); complete(&f, 4); CHECK_INT(draw(b), 0);
    // Another request is independent of that interlock.
    Json *answer = json_parsez(SSH_REQUESTS); json_set_str((Json *)json_at(json_get(answer, "requests"), 0), "id", "another-request");
    char *other = json_serialize(answer, false); json_free(answer); f.steps[5].answer = other;
    ssh_approvals_refresh(b); complete(&f, 5); tb = draw(b); CHECK(tb != 0);
    f.steps[6].answer = "{\"request\":{\"id\":\"another-request\",\"status\":\"denied\"}}";
    ssh_approvals_action(b, SSH_DENY, tb); entered(&f, 6); CHECK_STR(f.steps[6].url, "https://example.com/api/v1/ssh/requests/another-request/decision"); complete(&f, 6);
    CHECK_INT(draw(b), 0); free(other); ssh_approvals_free(b); clear(&f);
}
static void refresh_cooldown_refusal_and_scope(void) {
    Fixture f; init(&f); SshApprovals *a = ssh_approvals_new(&f.owner), *b = ssh_approvals_new(&f.owner);
    f.steps[0].status = 429; f.steps[0].retry = "3600";
    ssh_approvals_visible(a, true); entered(&f, 0); Request *first = a->read; ssh_approvals_refresh(a); CHECK(a->read == first);
    ssh_approvals_visible(b, true); entered(&f, 1); complete(&f, 0); double until = queue(a)->retry_until;
    CHECK(until >= (double)time(NULL) + 3598); complete(&f, 1); CHECK(!queue(b)->fresh); CHECK(queue(b)->retry_until == until);
    ssh_approvals_visible(a, false); ssh_approvals_visible(a, true); ssh_approvals_refresh(b); CHECK(a->read == NULL); CHECK(b->read == NULL);
    ssh_approvals_free(a); a = ssh_approvals_new(&f.owner); ssh_approvals_visible(a, true); CHECK(a->read == NULL);
    queue(a)->retry_until = 0; ssh_approvals_refresh(a); complete(&f, 2); CHECK(queue(a)->fresh); CHECK(draw(a));
    // Access revocation invalidates both views and suppresses an older successful read.
    ssh_approvals_refresh(a); entered(&f, 3); ssh_approvals_refresh(b); entered(&f, 4);
    f.steps[3].status = 403; complete(&f, 3); complete(&f, 4); CHECK_INT(json_count(queue(b)->rows), 0); CHECK_INT(draw(b), 0);
    queue(a)->retry_until = 0; ssh_approvals_refresh(a); entered(&f, 5);
    g_store.device.permission = "read"; complete(&f, 5); CHECK_INT(draw(a), 0);
    g_store.device.permission = "admin"; queue(a)->retry_until = 0;
    ssh_approvals_refresh(a); entered(&f, 6); ApiClient *original = g_store.client; g_store.client = NULL; complete(&f, 6);
    CHECK_INT(draw(a), 0); g_store.client = original;
    ssh_approvals_refresh(a); entered(&f, 7); ssh_approvals_free(a); f.steps[7].status = 429; f.steps[7].retry = "7200"; complete(&f, 7);
    CHECK(queue(b)->retry_until >= (double)time(NULL) + 7198);
    ssh_approvals_free(b); clear(&f);
}
static void expiry_refused_and_stale_actions(void) {
    Fixture f; init(&f); SshApprovals *s = ssh_approvals_new(&f.owner); ssh_approvals_visible(s, true); complete(&f, 0);
    intptr_t old = draw(s), token = draw(s); ssh_approvals_action(s, SSH_APPROVE, old); CHECK(s->write == NULL);
    Json *r = (Json *)json_get(ssh_approval_row(queue(s), "fake-request"), "request"); json_set_num(r, "expiresAt", 1);
    ssh_approvals_action(s, SSH_APPROVE, token); CHECK(s->write == NULL); CHECK_INT(draw(s), 0);
    ssh_approvals_refresh(s); complete(&f, 1); token = draw(s);
    f.steps[2].status = 400; f.steps[2].answer = "{\"error\":\"SSH request is no longer awaiting approval\"}";
    ssh_approvals_action(s, SSH_DENY, token); complete(&f, 2); CHECK(!queue(s)->fresh); CHECK(queue(s)->error != NULL); CHECK_INT(draw(s), 0);
    queue(s)->retry_until = 0; f.steps[3].answer = "{\"requests\":[]}";
    ssh_approvals_refresh(s); complete(&f, 3); CHECK_INT(ssh_approvals_badge(), 0); CHECK_INT(draw(s), 0);
    ssh_approvals_visible(s, false); ssh_approvals_action(s, SSH_APPROVE, token); CHECK(s->write == NULL);
    ssh_approvals_free(s); clear(&f);
}
static void refused_metadata_preserves_interlocks(void) {
    for (int inflight = 0; inflight < 2; inflight++) {
        Fixture f; init(&f); SshApprovals *a = ssh_approvals_new(&f.owner), *b = ssh_approvals_new(&f.owner);
        ssh_approvals_visible(a, true); complete(&f, 0); ssh_approvals_visible(b, true); complete(&f, 1);
        intptr_t token = draw(a); f.steps[2].fail = true; ssh_approvals_action(a, SSH_APPROVE, token); entered(&f, 2);
        f.steps[3].status = 403; ssh_approvals_refresh(b); entered(&f, 3);
        if (inflight) { complete(&f, 3); complete(&f, 2); } else { complete(&f, 2); complete(&f, 3); }
        CHECK_INT(json_count(queue(b)->rows), 0); CHECK(!json_is_null(json_get(queue(b)->decisions, "fake-request")));
        queue(b)->retry_until = 0; ssh_approvals_refresh(b); complete(&f, 4);
        CHECK(queue(b)->fresh); CHECK_INT(draw(b), 0); ssh_approvals_action(b, SSH_DENY, token); CHECK(b->write == NULL);
        ssh_approvals_free(a); ssh_approvals_free(b); clear(&f);
    }
}
static void restored_metadata_rebinds_and_keeps_interlocks(void) {
    Fixture f; init(&f); SshApprovals *s = ssh_approvals_new(&f.owner);
    ssh_approvals_visible(s, true); complete(&f, 0); intptr_t old = draw(s); CHECK(old != 0);
    ssh_approvals_refresh(s); entered(&f, 1);
    g_store.device.label = "Verified device";
    CHECK_INT(draw(s), 0); entered(&f, 2); CHECK(!queue(s)->fresh);
    ssh_approvals_action(s, SSH_APPROVE, old); CHECK(s->write == NULL);
    complete(&f, 1); CHECK(!queue(s)->fresh); // The cached-device response cannot populate the new scope.
    complete(&f, 2); intptr_t token = draw(s); CHECK(token != 0);
    f.steps[3].fail = true; ssh_approvals_action(s, SSH_APPROVE, token); entered(&f, 3);
    g_store.device.expires_at = 9999999999999.0;
    CHECK_INT(draw(s), 0); entered(&f, 4);
    CHECK(!json_is_null(json_get(queue(s)->decisions, "fake-request")));
    complete(&f, 3); CHECK(!queue(s)->fresh);
    complete(&f, 4); CHECK(queue(s)->fresh); CHECK_INT(draw(s), 0);
    SshApprovals *b = ssh_approvals_new(&f.owner); ssh_approvals_visible(b, true); complete(&f, 5);
    CHECK_INT(draw(b), 0); ssh_approvals_free(b);
    // Returning to a previously cached scope still requires a new read and retains the interlock.
    g_store.device.expires_at = 0;
    CHECK_INT(draw(s), 0); entered(&f, 6); CHECK(!queue(s)->fresh);
    complete(&f, 6); CHECK(queue(s)->fresh); CHECK_INT(draw(s), 0);
    ssh_approvals_free(s); clear(&f);
}
static void findings_cooldown_does_not_delay_ssh(void) {
    Fixture f; init(&f);
    wchar_t dir[MAX_PATH]; GetTempPathW(MAX_PATH, dir); wcscat(dir, L"briareus-ssh-poll-tests");
    g_store.cache = cache_new(dir); cache_remove_all(g_store.cache);
    Screen *base = findings_screen_new(); FindingsScreen *s = (FindingsScreen *)base;
    Route routes[] = {{"GET", "/ssh/requests", "manage"}, {"POST", "/ssh/requests/{id}/decision", "manage"}, {"GET", "/projects", "read"}};
    g_store.routes = routes; g_store.route_count = 3;
    screen_fixture_pane(&f.screen, base); base->vt->visible(base, true); complete(&f, 0);
    f.steps[1].status = 429; f.steps[1].retry = "3600";
    base->vt->timer(base, TIMER_POLL); complete(&f, 1);
    CHECK_STR(f.steps[1].url, "https://example.com/api/v1/projects");
    CHECK_INT(s->poller.retry_after, 3600); CHECK_INT(s->ssh->poller.base_ms, 7000);
    base->vt->timer(base, SSH_POLL); complete(&f, 2);
    CHECK_STR(f.steps[2].url, "https://example.com/api/v1/ssh/requests");
    CHECK_INT(s->poller.retry_after, 3600); CHECK_INT(s->ssh->poller.retry_after, -1);
    f.steps[3].status = 429; f.steps[3].retry = "3600";
    base->vt->timer(base, SSH_POLL); complete(&f, 3);
    base->vt->timer(base, SSH_POLL); CHECK(s->ssh->read == NULL); CHECK_INT(f.calls, 4);
    base->vt->visible(base, false); CHECK(!s->ssh->poller.running);
    base->vt->timer(base, SSH_POLL); CHECK_INT(f.calls, 4);
    base->vt->destroy(base); cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(dir); clear(&f);
}
void app_ssh_approval_tests(void) {
    screen_fixture_prepare();
    test_run("SSH restored metadata rebinds without unlocking decisions", restored_metadata_rebinds_and_keeps_interlocks);
    test_run("Findings cooldown leaves SSH polling independent", findings_cooldown_does_not_delay_ssh);
    test_run("SSH refusal redacts metadata but retains decision interlocks", refused_metadata_preserves_interlocks);
    test_run("SSH approval catalog/token gating and exact wire", gating_and_exact_wire);
    test_run("SSH approval multiple views and uncertain teardown", multiple_views_uncertain_and_teardown);
    test_run("SSH approval cooldown and stale scope completions", refresh_cooldown_refusal_and_scope);
    test_run("SSH approval expiry, refused and stale clicks", expiry_refused_and_stale_actions);
}
