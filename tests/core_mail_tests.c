// Contract fixtures pinned to nadinyamaui/briareus PR #120, f189a4228858c8edb9af2566f38e12c1a77708f2.
#include "mail.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *LIST = "{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"a@example.com\",\"label\":\"Work\",\"enabled\":true,\"syncDays\":60,\"status\":\"reauth\",\"syncing\":false,\"lastSyncAt\":1791400000000,\"lastSyncError\":\"Access revoked\",\"messages\":12,\"unread\":3,\"createdAt\":1791300000000,\"updatedAt\":1791400000001}],\"providers\":[\"gmail\"],\"callbackUrl\":\"https://core.example/oauth/mail/callback\",\"defaults\":{\"label\":\"\",\"enabled\":true,\"syncDays\":30}}";
static MailAccounts accounts(void) { MailAccounts a; Json *j = json_parsez(LIST); CHECK(mail_accounts_parse(j, &a)); json_free(j); return a; }
static Json *start_json(bool server) {
    Json *j = json_object(); json_set_str(j, "url", "https://accounts.google.com/o/oauth2/v2/auth?state=s%2B1");
    json_set_str(j, "state", "s+1"); json_set_str(j, "redirectUri", "http://127.0.0.1:8888/callback");
    json_set_bool(j, "finishesOnServer", server); json_set_num(j, "expiresAt", 1791400900000); return j;
}
static void models(void) {
    MailAccounts a = accounts(); const MailAccount *m = mail_account_find(&a, 7);
    CHECK(m != NULL); CHECK_INT(m->sync_days, 60); CHECK_INT(m->messages, 12); CHECK_INT(m->unread, 3);
    CHECK_STR(m->label, "Work"); CHECK_STR(m->email, "a@example.com"); CHECK_STR(m->last_sync_error, "Access revoked");
    CHECK(m->enabled && !m->syncing && m->last_sync_at == 1791400000000 && m->created_at == 1791300000000 && m->updated_at == 1791400000001);
    CHECK(mail_provider_available(&a, "gmail") && !mail_provider_available(&a, "outlook") && !mail_provider_available(&a, "future"));
    CHECK(mail_account_find(&a, 8) == NULL); CHECK(a.default_enabled); CHECK_INT(a.default_sync_days, 30); mail_accounts_free(&a);
    Json *j = json_parsez("{\"accounts\":[{\"id\":8,\"provider\":\"future\",\"email\":\"b@example.com\",\"credentials\":\"never retain\",\"status\":\"new-status\",\"syncing\":true}],\"providers\":[\"outlook\",\"future\",null],\"defaults\":{\"syncDays\":999,\"enabled\":false}}");
    CHECK(mail_accounts_parse(j, &a)); json_free(j);
    CHECK(!a.gmail && a.outlook && !a.default_enabled); CHECK_INT(a.default_sync_days, 30);
    CHECK_STR(a.accounts[0].status, "new-status"); CHECK(a.accounts[0].syncing); CHECK(a.accounts[0].last_sync_at == 0); mail_accounts_free(&a);
    j = json_parsez("{\"accounts\":[],\"providers\":[]}"); CHECK(mail_accounts_parse(j, &a)); CHECK(!a.gmail && !a.outlook); mail_accounts_free(&a); json_free(j);
    CHECK(!mail_accounts_parse(NULL, &a));
    j = json_parsez("{\"accounts\":[{\"id\":1,\"provider\":\"gmail\",\"email\":\"x\"},{}],\"providers\":[]}");
    CHECK(!mail_accounts_parse(j, &a)); CHECK(a.accounts == NULL && a.count == 0); json_free(j);
    MailAccount bad; j = json_parsez("{\"id\":0}"); CHECK(!mail_account_parse(j, &bad)); json_free(j);
}
static void settings(void) {
    const char *bad[] = { "", "0", "366", "-1", "+1", "1.5", "NaN", "1e2", "2x", "999999999999999999999999" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(mail_settings_body("Work", true, bad[i]) == NULL);
    Json *j = mail_settings_body(NULL, false, " 365 "); CHECK(j != NULL); CHECK_INT(json_count(j), 3);
    CHECK_STR(json_str(json_get(j, "label")), ""); CHECK(json_bool_is(json_get(j, "enabled"), false)); CHECK_INT(json_int_or(json_get(j, "syncDays"), 0), 365);
    CHECK(json_is_null(json_get(j, "credentials"))); CHECK(json_is_null(json_get(j, "accessToken"))); json_free(j);
    j = mail_settings_body("Work", true, "1"); CHECK(j != NULL); json_free(j);
}
static void callbacks(void) {
    MailAccounts a = accounts(); Json *j = start_json(false); MailSignIn s;
    CHECK(mail_sign_in_parse(j, "gmail", 7, &a, &s)); json_free(j); mail_accounts_free(&a);
    // Snapshot owns its data even after the caller replaces the list.
    CHECK_STR(s.before.accounts[0].status, "reauth"); CHECK_INT(s.account_id, 7);
    Json *body = mail_sign_in_finish(&s, "http://127.0.0.1:8888/callback?code=c%2B1&state=s%2B1&scope=mail", 1791400000000);
    CHECK(body != NULL); CHECK_STR(json_str(json_get(body, "state")), "s+1"); CHECK_STR(json_str(json_get(body, "code")), "c+1"); json_free(body);
    const char *bad[] = {
        "https://evil.example/callback?state=s%2B1&code=c", "http://127.0.0.1:8889/callback?state=s%2B1&code=c",
        "http://127.0.0.1:8888/other?state=s%2B1&code=c", "http://127.0.0.1:8888/callback?state=wrong&code=c",
        "http://127.0.0.1:8888/callback?state=s%2B1&code=", "http://127.0.0.1:8888/callback?state=s%2B1&code=c&code=d",
        "http://127.0.0.1:8888/callback?state=s%2B1&state=s%2B1&code=c", "http://127.0.0.1:8888/callback?state=s%2B1&code=c&error=access_denied",
        "http://127.0.0.1:8888/callback?state=s%2B1&code=%00", "http://127.0.0.1:8888/callback?state=s%2B1&code=%0a",
        "http://127.0.0.1:8888/callback?state=s%2B1&code=%XX", "http://127.0.0.1:8888/callback?state=s%2B1&code=%",
        "http://127.0.0.1:8888/callback?state=s%2B1&code=c#state=wrong", "http://127.0.0.1:8888/callback?state=s%2B1&code=c&bare",
        "http://127.0.0.1:8888/callback", "http://127.0.0.1:8888/callback?code=c", "http://127.0.0.1:8888/callback?state=s%2B1&code=c+\177",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(mail_sign_in_finish(&s, bad[i], 1791400000000) == NULL);
    CHECK(mail_sign_in_finish(&s, NULL, 1791400000000) == NULL);
    CHECK(mail_sign_in_finish(&s, "http://127.0.0.1:8888/callback?state=s%2B1&code=c", s.expires_at) == NULL);
    CHECK(mail_sign_in_finish(&s, "http://127.0.0.1:8888/callback?state=s%2B1&code=c", NAN) == NULL);
    s.server_finish = true; CHECK(mail_sign_in_finish(&s, "http://127.0.0.1:8888/callback?state=s%2B1&code=c", 1) == NULL);
    mail_sign_in_free(&s); CHECK(s.state == NULL);
}
static void invalid_starts(void) {
    MailAccounts a = accounts(); MailSignIn s; Json *j = start_json(true);
    CHECK(!mail_sign_in_parse(j, "outlook", 0, &a, &s));
    const char *bad[] = { "javascript:alert(1)", "http://remote.example/x", "https://u@host/x", "https://host/x#fragment", "https://host/x?query", "http://127.0.0.1:0/x", "http://localhost:65536/x", "http://localhost:8x", "http://localhost:8/a b", "http://localhost:8/a\\b" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { json_set_str(j, "redirectUri", bad[i]); CHECK(!mail_sign_in_parse(j, "gmail", 0, &a, &s)); }
    json_set_str(j, "redirectUri", "https://core.example/oauth/mail/callback");
    CHECK(mail_sign_in_parse(j, "gmail", 0, &a, &s)); mail_sign_in_free(&s);
    json_set_str(j, "redirectUri", "http://localhost:8123/callback");
    CHECK(mail_sign_in_parse(j, "gmail", 0, &a, &s)); mail_sign_in_free(&s);
    json_object_remove(j, "finishesOnServer"); CHECK(!mail_sign_in_parse(j, "gmail", 0, &a, &s)); json_set_bool(j, "finishesOnServer", false);
    json_set_num(j, "expiresAt", 0); CHECK(!mail_sign_in_parse(j, "gmail", 0, &a, &s)); json_set_num(j, "expiresAt", 1);
    json_set_str(j, "state", ""); CHECK(!mail_sign_in_parse(j, "gmail", 0, &a, &s)); json_set_str(j, "state", "state");
    json_set_str(j, "url", "http://evil.example/"); CHECK(!mail_sign_in_parse(j, "gmail", 0, &a, &s));
    json_free(j); mail_accounts_free(&a);
}
static void completion(void) {
    MailAccounts a = accounts(); Json *j = start_json(true); MailSignIn s;
    CHECK(mail_sign_in_parse(j, "gmail", 7, &a, &s)); CHECK(!mail_sign_in_completed(&s, &a));
    free(a.accounts[0].status); a.accounts[0].status = xstrdup("connected");
    CHECK(mail_sign_in_completed(&s, &a)); mail_sign_in_free(&s);
    CHECK(mail_sign_in_parse(j, "gmail", 7, &a, &s));
    a.accounts[0].updated_at += 600000; a.accounts[0].syncing = true;
    CHECK(!mail_sign_in_completed(&s, &a)); // A sync timestamp is not an OAuth completion.
    a.accounts[0].id = 8; CHECK(!mail_sign_in_completed(&s, &a)); // Wrong account cannot satisfy reauth.
    mail_sign_in_free(&s);
    CHECK(mail_sign_in_parse(j, "gmail", 0, &a, &s)); a.accounts[0].id = 9;
    free(a.accounts[0].provider); a.accounts[0].provider = xstrdup("outlook"); CHECK(!mail_sign_in_completed(&s, &a));
    free(a.accounts[0].provider); a.accounts[0].provider = xstrdup("gmail"); CHECK(mail_sign_in_completed(&s, &a));
    mail_sign_in_free(&s); mail_accounts_free(&a); json_free(j);
}
static void errors(void) {
    CHECK(strstr(mail_error_message(409, true), "different mailbox") != NULL);
    CHECK(strstr(mail_error_message(409, false), "sign-in again") != NULL);
    CHECK(strstr(mail_error_message(400, true), "expired") != NULL);
    const int codes[] = { 0, 400, 401, 403, 404, 429, 503, 500 };
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++) CHECK(!str_empty(mail_error_message(codes[i], false)));
}
void mail_tests(void) {
    test_run("mail accounts are credential-free and providers come from the server", models);
    test_run("mail settings validate the sync window and send only editable fields", settings);
    test_run("mail callback validates destination state expiry and single-use parameters", callbacks);
    test_run("mail sign-in refuses malformed and unavailable provider starts", invalid_starts);
    test_run("mail server completion requires the right account and ignores sync timestamps", completion);
    test_run("mail failures explain reauth unavailable provider and wrong mailbox", errors);
}
