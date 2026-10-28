#include "mail_wake.h"
#include "api.h"
#include "str.h"
#include "test.h"
#include "suites.h"
#include <stdlib.h>
#include <string.h>

static const char *RECORD = "{\"rules\":[{\"id\":\"abc123\",\"account\":7,\"enabled\":true,\"to\":\"ops@example.com\",\"from\":\"\",\"subject\":\"Alert\",\"since\":1720000000000}],\"perHour\":30,\"maxTurns\":10}";
static MailWake wake(const char *text) {
    MailWake w = {0}; Json *j = json_parsez(text); CHECK(mail_wake_read(&w, j)); json_free(j); return w;
}
static MailAccounts accounts(void) {
    Json *j = json_parsez("{\"providers\":[\"gmail\"],\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"me@example.com\",\"enabled\":true,\"status\":\"connected\",\"access\":\"read\"},{\"id\":8,\"provider\":\"outlook\",\"email\":\"other@example.com\",\"enabled\":true,\"status\":\"connected\"}]}");
    MailAccounts a; CHECK(mail_accounts_parse(j, &a)); json_free(j); return a;
}
static void read_defaults(void) {
    MailWake w = wake("{}"); CHECK_INT(json_count(w.rules), 0); CHECK_STR(w.caps[0], "30"); CHECK_STR(w.caps[1], "10"); CHECK(!mail_wake_dirty(&w));
    Json *j = json_parsez("{\"rules\":[{\"account\":7}],\"perHour\":1,\"maxTurns\":0}");
    CHECK(mail_wake_read(&w, j)); CHECK_STR(w.caps[0], "1"); CHECK_STR(w.caps[1], "0");
    CHECK(json_bool_is(json_get(json_at(w.rules, 0), "enabled"), true)); CHECK_STR(json_str(json_get(json_at(w.rules, 0), "to")), "");
    CHECK(mail_wake_active(j)); json_free(j);
    j = json_parsez("{\"rules\":[{\"account\":7,\"enabled\":false}]}"); CHECK(!mail_wake_active(j)); json_free(j);
    CHECK(!mail_wake_active(NULL));
    const char *bad[] = {"[]", "{\"rules\":false}", "{\"rules\":[false]}", "{\"rules\":[{\"account\":\"7\"}]}", "{\"rules\":[{\"account\":1.5}]}", "{\"rules\":[{\"account\":7,\"enabled\":\"false\"}]}", "{\"rules\":[{\"account\":7,\"to\":3}]}", "{\"rules\":[{\"account\":7,\"id\":5}]}", "{\"perHour\":\"30\"}", "{\"maxTurns\":false}", "{\"maxTurns\":1.5}"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        j = json_parsez(bad[i]); CHECK(!mail_wake_read(&w, j)); CHECK_STR(w.caps[1], "0"); CHECK_INT(json_count(w.rules), 1); json_free(j);
    }
    mail_wake_free(&w);
}
static void bodies_and_gates(void) {
    MailWake w = wake(RECORD); MailAccounts a = accounts(); char *error = NULL;
    CHECK(mail_wake_mailbox_usable(&a, 7)); CHECK(!mail_wake_mailbox_usable(&a, 8));
    Json *body = mail_wake_body(&w, &a, 1, &error);
    Json *expected = json_parsez("{\"rules\":[{\"id\":\"abc123\",\"account\":7,\"enabled\":true,\"to\":\"ops@example.com\",\"from\":\"\",\"subject\":\"Alert\"}],\"perHour\":30,\"maxTurns\":10}");
    CHECK(body != NULL); CHECK(error == NULL); CHECK(json_equal(body, expected)); CHECK(mail_wake_saved_matches(&w, expected));
    json_set_num(expected, "maxTurns", 11); CHECK(!mail_wake_saved_matches(&w, expected)); json_free(expected); json_free(body);
    CHECK(mail_wake_body(&w, &a, 0, &error) == NULL); CHECK(error != NULL); free(error);
    CHECK(mail_wake_body(&w, &a, -1, &error) == NULL); free(error);
    Json *r = (Json *)json_at(w.rules, 0); json_set_num(r, "account", 8);
    CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
    json_set_num(r, "account", 999); json_set_bool(r, "enabled", false);
    body = mail_wake_body(&w, NULL, 0, &error); CHECK(body != NULL); CHECK_INT(json_int_or(json_get(json_at(json_get(body, "rules"), 0), "account"), 0), 999); json_free(body);
    json_set_num(r, "account", 7); json_set_bool(r, "enabled", true);
    a.accounts[0].enabled = false; CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error); a.accounts[0].enabled = true;
    free(a.accounts[0].status); a.accounts[0].status = xstrdup("disconnected"); CHECK(!mail_wake_mailbox_usable(&a, 7));
    json_set_num(r, "account", 0); CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
    Json *settings = json_parsez("{\"projects\":[{\"repo\":\"o/r\",\"mailToolsEnabled\":false},{\"repo\":\"o/s\",\"mailToolsEnabled\":true}]}");
    CHECK_INT(mail_wake_tools(settings, "o/r"), 0); CHECK_INT(mail_wake_tools(settings, "o/s"), 1); CHECK_INT(mail_wake_tools(settings, "missing"), -1); json_free(settings);
    settings = json_parsez("[{\"repo\":\"o/r\"}]"); CHECK_INT(mail_wake_tools(settings, "o/r"), -1); CHECK_INT(mail_wake_tools(settings, NULL), -1); json_free(settings);
    mail_wake_free(&w); mail_accounts_free(&a);
}
static void rule_limits_and_dirty(void) {
    MailWake w = wake("{}"); MailAccounts a = accounts(); char *error = NULL;
    Json *body = mail_wake_body(&w, &a, 1, &error); CHECK_INT(json_count(json_get(body, "rules")), 0); json_free(body);
    for (int i = 0; i < 20; i++) CHECK(mail_wake_add(&w, 7));
    CHECK(mail_wake_dirty(&w)); CHECK(!mail_wake_add(&w, 7));
    body = mail_wake_body(&w, &a, 1, &error); CHECK_INT(json_count(json_get(body, "rules")), 20); json_free(body);
    json_array_push(w.rules, json_clone(json_at(w.rules, 0)));
    CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
    Json *record = json_object(); json_object_set(record, "rules", json_clone(w.rules)); CHECK(!mail_wake_read(&w, record)); json_free(record);
    mail_wake_remove(&w, 20); mail_wake_remove(&w, 999); CHECK_INT(json_count(w.rules), 20);
    mail_wake_discard(&w); CHECK(!mail_wake_dirty(&w)); CHECK_INT(json_count(w.rules), 0);
    free(w.caps[0]); w.caps[0] = xstrdup("600"); CHECK(mail_wake_dirty(&w)); mail_wake_discard(&w); CHECK_STR(w.caps[0], "30");
    mail_wake_free(&w); w = wake(RECORD); json_array_push(w.rules, json_clone(json_at(w.rules, 0)));
    CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
    mail_wake_free(&w); mail_accounts_free(&a);
}
static void filters_and_caps(void) {
    MailWake w = wake(RECORD); MailAccounts a = accounts(); Json *r = (Json *)json_at(w.rules, 0); char *error = NULL;
    const char *bad[] = {"two@example.com,other@example.com", "Name <a@b.com>", "a@b", "@b.com", "a@.com", "a@b.", "a@b\xC2\xA0.com", "a\xED\xA0\x80@b.com", "\xF0\x80\x80\x80"};
    for (int f = 0; f < 2; f++) {
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            json_set_str(r, mail_wake_filter_keys[f], bad[i]); CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
        }
        json_set_str(r, mail_wake_filter_keys[f], "");
    }
    json_set_str(r, "to", "\xEF\xBB\xBF ops@example.com\xC2\xA0");
    Json *body = mail_wake_body(&w, &a, 1, &error); CHECK(body != NULL); CHECK_STR(json_str(json_get(json_at(json_get(body, "rules"), 0), "to")), "ops@example.com"); json_free(body);
    char address[322]; memset(address, 'a', 314); strcpy(address + 314, "@b.com");
    json_set_str(r, "from", address); body = mail_wake_body(&w, &a, 1, &error); CHECK(body != NULL); json_free(body);
    memmove(address + 1, address, 321); address[0] = 'a'; json_set_str(r, "from", address); CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error); json_set_str(r, "from", "");
    Str text; str_init(&text); for (int i = 0; i < 160; i++) str_appendz(&text, "\xF0\x9F\x98\x80");
    json_set_str(r, "subject", text.data); body = mail_wake_body(&w, &a, 1, &error); CHECK(body != NULL); json_free(body);
    str_appendc(&text, 'x'); json_set_str(r, "subject", text.data); CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error); str_free(&text); json_set_str(r, "subject", "");
    const char *invalid[] = {"0", "-1", "1.5", "1e2", "", "NaN", "1001", "99999999999999999999"};
    for (int c = 0; c < 2; c++) {
        for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
            free(w.caps[c]); w.caps[c] = xstrdup(invalid[i]); CHECK(mail_wake_body(&w, &a, 1, &error) == NULL); free(error);
        }
        free(w.caps[c]); w.caps[c] = xstrdup(c ? "1000" : "600");
    }
    body = mail_wake_body(&w, &a, 1, &error); CHECK(body != NULL); json_free(body);
    mail_wake_free(&w); mail_accounts_free(&a);
}
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *bytes, size_t size, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; (void)error; int *calls = ctx; (*calls)++;
    CHECK_STR(url, "https://example.com/api/v1/sessions/s%2F%2B/mail-wake");
    CHECK_STR(method, *calls == 1 ? "GET" : "PUT");
    if (*calls > 1) {
        char *text = xstrndup(bytes, size); Json *j = json_parsez(text); free(text);
        CHECK_INT(json_count(j), 3); CHECK_INT(json_count(json_get(j, "rules")), 0); CHECK_INT(json_int_or(json_get(j, "maxTurns"), 0), 10); json_free(j);
    }
    *status = *calls == 3 ? 429 : 200; *type = xstrdup("application/json"); *retry = xstrdup("120");
    *response = xstrdup(*calls == 3 ? "{\"error\":\"slow down\"}" : "{\"rules\":[],\"perHour\":30,\"maxTurns\":10}"); *len = strlen(*response); return true;
}
static void wire(void) {
    ServerAddress addr; CHECK(server_address_parse("https://example.com", &addr)); ApiError e; api_error_init(&e);
    ApiClient *client = api_client_new(&addr, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &e); server_address_free(&addr);
    int calls = 0; api_client_set_transport(client, transport, &calls); Json *args = json_object(); json_set_str(args, "sessionId", "s/+");
    Json *answer = api_call(client, "session_mail_wake", args, 0, &e); CHECK(answer != NULL); json_free(answer);
    json_object_set(args, "rules", json_array()); json_set_num(args, "perHour", 30); json_set_num(args, "maxTurns", 10);
    answer = api_call(client, "set_session_mail_wake", args, 0, &e); CHECK(answer != NULL); json_free(answer);
    CHECK(api_call(client, "set_session_mail_wake", args, 0, &e) == NULL); CHECK_INT(e.status, 429); CHECK_INT((int)e.retry_after, 120); CHECK_INT(calls, 3);
    json_free(args); api_client_release(client); api_error_clear(&e);
}
void core_mail_wake_tests(void) {
    test_run("mail wake defaults, explicit zero, active summary and atomic malformed reads", read_defaults);
    test_run("mail wake exact body and project/provider/receiving mailbox gates", bodies_and_gates);
    test_run("mail wake 0/20/21 rules, duplicate ids, discard and dirty semantics", rule_limits_and_dirty);
    test_run("mail wake address, JS Unicode trim, UTF-16 and positive cap boundaries", filters_and_caps);
    test_run("mail wake exact GET/PUT routes, direct envelope and no 429 write retry", wire);
}
