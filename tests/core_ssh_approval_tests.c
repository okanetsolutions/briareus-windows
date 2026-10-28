#include "test.h"
#include "suites.h"
#include "ssh_approval.h"
#include "str.h"
#include "fixtures/ssh_approval.h"
#include <stdlib.h>
#include <string.h>

static Device device = {.permission = "manage"};
static const Json *request(SshApprovalQueue *q) { return json_get(ssh_approval_row(q, "fake-request"), "request"); }
static void load(SshApprovalQueue *q) { Json *answer = json_parsez(SSH_REQUESTS); CHECK(ssh_approval_read(q, answer, q->generation)); json_free(answer); }
static void metadata(void) {
    SshApprovalQueue *queues = NULL, *q = ssh_approval_queue(&queues, "device/project");
    CHECK(ssh_approval_queue(&queues, "device/project") == q);
    CHECK(ssh_approval_queue(&queues, "other") != q); load(q);
    CHECK(!ssh_approval_elevated(request(q))); CHECK_INT(ssh_approval_waiting(q, 1000), 1);
    Json *answer = json_parsez(SSH_REQUESTS), *r = (Json *)json_at(json_get(answer, "requests"), 0);
    json_set_bool(r, "sudo", true); json_set_bool(r, "unattended", true);
    json_set_str(r, "password", "never-render-this"); json_set_str(r, "privateKey", "never-render-this");
    Json *u = json_parsez("{\"sourcePath\":\"/fixture/report\",\"destination\":\"/tmp/report\",\"size\":123,\"sha256\":\"fixture-hash\",\"contents\":\"never-render-this\"}"); json_object_set(r, "upload", u);
    CHECK(ssh_approval_read(q, answer, q->generation)); CHECK(ssh_approval_elevated(request(q)));
    char *detail = ssh_approval_detail(request(q), 1000);
    CHECK(strstr(detail, "as root")); CHECK(strstr(detail, "printf fixture")); CHECK(strstr(detail, "/fixture/report"));
    CHECK(strstr(detail, "/tmp/report")); CHECK(strstr(detail, "123 bytes")); CHECK(strstr(detail, "fixture-hash"));
    CHECK(strstr(detail, "operator@fixture.invalid:22")); CHECK(strstr(detail, "fake-session")); CHECK(strstr(detail, "fake-request"));
    CHECK(!strstr(detail, "never-render-this")); free(detail);
    char *serialized = json_serialize(q->rows, true); CHECK(!strstr(serialized, "never-render-this")); free(serialized);
    json_set_bool(r, "sudo", false); CHECK(ssh_approval_read(q, answer, q->generation)); CHECK(!ssh_approval_elevated(request(q)));
    detail = ssh_approval_detail(request(q), 4102444801.0); CHECK(strstr(detail, "expired")); free(detail);
    json_array_push((Json *)json_get(answer, "requests"), json_object()); CHECK(ssh_approval_read(q, answer, q->generation)); CHECK_INT(json_count(q->rows), 1);
    json_free(answer); answer = json_object(); CHECK(!ssh_approval_read(q, answer, q->generation)); json_free(answer);
    ssh_approval_free(&queues); CHECK(queues == NULL);
}
static void eligibility_and_outcomes(void) {
    SshApprovalQueue *queues = NULL, *q = ssh_approval_queue(&queues, "scope"); load(q);
    const Json *r = request(q); CHECK(ssh_approval_eligible(q, r, &device, 1000));
    Device read = {.permission = "read"}; CHECK(!ssh_approval_eligible(q, r, &read, 1000));
    char *repos[] = {"other/repo"}; Device limited = {.permission = "manage", .repos = repos, .repo_count = 1};
    CHECK(!ssh_approval_eligible(q, r, &limited, 1000)); repos[0] = "one/repo"; CHECK(ssh_approval_eligible(q, r, &limited, 1000));
    CHECK(!ssh_approval_eligible(q, r, &device, 4102444800.0));
    Json *stale = json_clone(r); json_set_str(stale, "command", "changed"); CHECK(!ssh_approval_begin(q, stale, &device, 1000)); json_free(stale);
    Json *sent = json_clone(r); unsigned long long old = q->generation;
    CHECK(ssh_approval_begin(q, r, &device, 1000)); CHECK(!ssh_approval_begin(q, r, &device, 1000));
    Json *answer = json_parsez(SSH_REQUESTS); CHECK(!ssh_approval_read(q, answer, old)); json_free(answer);
    ApiError error; api_error_init(&error);
    answer = json_parsez("{\"request\":{\"id\":\"fake-request\",\"status\":\"running\"}}");
    ssh_approval_finish(q, sent, true, answer, &error, 1000); CHECK_INT(ssh_approval_waiting(q, 1000), 0); json_free(answer);
    answer = json_parsez("{\"requests\":[]}"); CHECK(ssh_approval_read(q, answer, q->generation)); CHECK_INT(json_count(q->rows), 1); json_free(answer);
    // Every consequential result remains interlocked after a fresh read and independent scope changes.
    const char *statuses[] = {"denied", "completed", "failed", "pending"};
    for (size_t i = 0; i < 4; i++) {
        json_free(q->rows); q->rows = json_object(); json_free(q->decisions); q->decisions = json_object(); load(q); CHECK(ssh_approval_begin(q, request(q), &device, 1000));
        answer = json_object(); Json *reply = json_object(); json_set_str(reply, "id", "fake-request"); json_set_str(reply, "status", statuses[i]); json_object_set(answer, "request", reply);
        ssh_approval_finish(q, sent, true, answer, &error, 1000); json_free(answer);
        CHECK(!ssh_approval_eligible(q, request(q), &device, 1000));
        load(q); CHECK(!ssh_approval_eligible(q, request(q), &device, 1000));
    }
    // A read refusal redacts metadata while preserving the ID-only decision interlock.
    api_error_set(&error, API_HTTP, 403, "fixture access revoked", 0); ssh_approval_failure(q, &error, 1000);
    CHECK_INT(json_count(q->rows), 0); load(q); CHECK(!ssh_approval_eligible(q, request(q), &device, 1000));
    json_free(sent); api_error_clear(&error); ssh_approval_free(&queues);
}
static void refusals_and_cooldowns(void) {
    int codes[] = {400, 401, 403, 404, 429, 503};
    for (size_t i = 0; i < 6; i++) {
        SshApprovalQueue *queues = NULL, *q = ssh_approval_queue(&queues, "scope"); load(q);
        ApiError e; api_error_init(&e); api_error_set(&e, API_HTTP, codes[i], "fixture refusal", 3600);
        unsigned long long generation = q->generation; Json *answer = json_parsez(SSH_REQUESTS);
        ssh_approval_failure(q, &e, 1000); CHECK(!q->fresh); CHECK(q->retry_until == 4600);
        CHECK(!ssh_approval_read(q, answer, generation));
        api_error_set(&e, API_HTTP, 503, "older shorter deadline", 7); ssh_approval_failure(q, &e, 1001); CHECK(q->retry_until == 4600);
        CHECK(ssh_approval_read(q, answer, q->generation)); CHECK(q->retry_until == 4600);
        CHECK(!ssh_approval_begin(q, request(q), &device, 1002)); json_free(answer);
        q->retry_until = 0; Json *sent = json_clone(request(q)); CHECK(ssh_approval_begin(q, sent, &device, 5000));
        api_error_set(&e, API_HTTP, codes[i], "fixture refusal", -1);
        ssh_approval_finish(q, sent, false, NULL, &e, 5000); CHECK(!q->fresh);
        if (codes[i] == 401 || codes[i] == 403) CHECK_INT(json_count(q->rows), 0);
        else CHECK(!json_is_null(json_get(ssh_approval_row(q, "fake-request"), "result")));
        json_free(sent); api_error_clear(&e); ssh_approval_free(&queues);
    }
    CHECK(ssh_approval_body(NULL, "approve") == NULL); CHECK(ssh_approval_body("", "deny") == NULL); CHECK(ssh_approval_body("r", "other") == NULL);
    Json *body = ssh_approval_body("r", "deny"); CHECK_STR(json_str(json_get(body, "decision")), "deny"); CHECK_INT(json_count(body), 2); json_free(body);
}
void core_ssh_approval_tests(void) {
    test_run("SSH approval public metadata and elevation", metadata);
    test_run("SSH approval eligibility and outcome interlocks", eligibility_and_outcomes);
    test_run("SSH approval refusals and monotonic cooldowns", refusals_and_cooldowns);
}
