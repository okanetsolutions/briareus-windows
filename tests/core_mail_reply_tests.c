#include "mail_reply.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static void validation(void) {
    const char *bad[] = {NULL, "", " \t\r\n", "\xc2\xa0\xe3\x80\x80\xef\xbb\xbf", "\xc0\x80", "\xf0\x9f", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe0\x80\x80", "\x80"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(mail_reply_body(bad[i]) == NULL);
    Json *body = mail_reply_body("  Thanks,\nAna \xf0\x9f\x98\x80\n"); CHECK(body != NULL);
    CHECK_STR(json_str(json_get(body, "text")), "  Thanks,\nAna \xf0\x9f\x98\x80\n"); CHECK_INT(json_count(body), 1); json_free(body);
    char *text = xmalloc(200005);
    for (int i = 0; i < 50000; i++) memcpy(text + i * 4, "\xf0\x9f\x98\x80", 4);
    text[200000] = 0; body = mail_reply_body(text); CHECK(body != NULL); json_free(body);
    text[200000] = 'a'; text[200001] = 0; CHECK(mail_reply_body(text) == NULL);
    memset(text, 'a', 100001); text[100000] = 0; body = mail_reply_body(text); CHECK(body != NULL); json_free(body);
    text[100000] = 'a'; text[100001] = 0; CHECK(mail_reply_body(text) == NULL); free(text);
    body = mail_reply_body("e\xcc\x81"); CHECK(body != NULL); json_free(body);
}

static void writable_identity(void) {
    Json *j = json_parsez("{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"a@example.com\",\"access\":\"read\",\"status\":\"connected\",\"createdAt\":123}],\"providers\":[\"gmail\"]}");
    MailAccounts a; CHECK(mail_accounts_parse(j, &a)); json_free(j);
    CHECK(!mail_account_writable(&a, 7)); CHECK(!mail_account_writable(&a, 8));
    MailAccount *account = &a.accounts[0]; CHECK(account->read_only); CHECK_STR(account->access, "read");
    free(account->access); account->access = xstrdup("manage"); account->read_only = false; CHECK(mail_account_writable(&a, 7));
    free(account->access); account->access = NULL; CHECK(mail_account_writable(&a, 7));
    free(account->status); account->status = xstrdup("reauth"); CHECK(!mail_account_writable(&a, 7));
    free(account->status); account->status = xstrdup("connected"); a.gmail = false; CHECK(!mail_account_writable(&a, 7)); a.gmail = true;
    account->access = xstrdup("future"); CHECK(!mail_account_writable(&a, 7));
    MailMessage m = {.account_id = 7, .id = "same/+=", .thread_id = "thread"};
    char *key = mail_reply_key(&a, &m); CHECK(key != NULL);
    m.id = "other"; char *other = mail_reply_key(&a, &m); CHECK(!str_eq(key, other)); free(other);
    m.id = "same/+="; account->created_at++; other = mail_reply_key(&a, &m); CHECK(!str_eq(key, other)); free(other);
    CHECK(mail_reply_same_account(account, account)); CHECK(!mail_reply_same_account(account, NULL));
    MailAccount replacement = *account; replacement.email = "replacement@example.com";
    CHECK(!mail_reply_same_account(account, &replacement)); free(key); mail_accounts_free(&a);
}

static void parsed_access(void) {
    const struct { const char *field, *access; bool writable; } cases[] = {
        {"", NULL, true},
        {",\"access\":\"manage\"", "manage", true},
        {",\"access\":\"read\"", "read", false},
        {",\"access\":\"future\"", "future", false},
        {",\"access\":\"\"", "", false},
        {",\"access\":false", "", false},
        {",\"access\":true", "", false},
        {",\"access\":123", "", false},
        {",\"access\":{}", "", false},
        {",\"access\":[]", "", false},
        {",\"access\":null", "", false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char *text = xstrfmt("{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"a@example.com\",\"status\":\"connected\"%s}],\"providers\":[\"gmail\"]}", cases[i].field);
        Json *j = json_parsez(text); free(text);
        MailAccounts a; CHECK(mail_accounts_parse(j, &a)); json_free(j);
        CHECK_INT(a.count, 1);
        if (a.count) CHECK_STR(a.accounts[0].access, cases[i].access);
        CHECK(mail_account_writable(&a, 7) == cases[i].writable);
        mail_accounts_free(&a);
    }
}

static void outcomes(void) {
    MailReply *drafts = NULL, *d = mail_reply_find(&drafts, "7:first", true);
    CHECK(mail_reply_find(&drafts, NULL, true) == NULL); CHECK(mail_reply_find(&drafts, "absent", false) == NULL);
    CHECK(mail_reply_find(&drafts, "7:first", false) == d); CHECK(!mail_reply_begin(d, 100));
    free(d->text); d->text = xstrdup("answer"); d->open = true;
    CHECK(mail_reply_begin(d, 100)); CHECK(!mail_reply_begin(d, 100)); CHECK(!mail_reply_reconcile(d));
    ApiError e; api_error_init(&e); Json *accepted = json_parsez("{\"status\":\"accepted\",\"action\":\"reply\"}");
    api_error_set(&e, API_HTTP, 403, "private provider details", -1);
    mail_reply_finish(d, false, false, NULL, &e, 100); CHECK(!d->uncertain); CHECK_STR(d->text, "answer"); CHECK(d->error != NULL);
    CHECK(mail_reply_begin(d, 100)); api_error_set(&e, API_HTTP, 429, NULL, 90);
    mail_reply_finish(d, false, false, NULL, &e, 100); CHECK_INT(d->retry_until, 90100); CHECK(!mail_reply_begin(d, 90099));
    CHECK(mail_reply_begin(d, 90100)); api_error_set(&e, API_HTTP, 502, NULL, -1);
    mail_reply_finish(d, false, false, NULL, &e, 90100); CHECK(d->uncertain); CHECK(!mail_reply_begin(d, 1000000)); CHECK_STR(d->text, "answer");
    CHECK(mail_reply_reconcile(d)); CHECK(mail_reply_begin(d, 90100));
    mail_reply_finish(d, true, true, accepted, &e, 90100); CHECK(d->uncertain); CHECK_STR(d->text, "answer");
    CHECK(mail_reply_reconcile(d)); CHECK(mail_reply_begin(d, 90100)); api_error_set(&e, API_NETWORK, 0, NULL, -1);
    mail_reply_finish(d, false, false, NULL, &e, 90100); CHECK(d->uncertain);
    CHECK(mail_reply_reconcile(d)); CHECK(mail_reply_begin(d, 90100)); api_error_set(&e, API_HTTP, 408, NULL, -1);
    mail_reply_finish(d, false, false, NULL, &e, 90100); CHECK(d->uncertain); CHECK_STR(d->text, "answer");
    CHECK(mail_reply_reconcile(d)); CHECK(mail_reply_begin(d, 90100));
    Json *wrong = json_parsez("{\"status\":\"accepted\",\"action\":\"send\"}");
    mail_reply_finish(d, false, true, wrong, &e, 90100); CHECK(d->uncertain); json_free(wrong);
    CHECK(mail_reply_reconcile(d)); CHECK(mail_reply_begin(d, 90100));
    MailReply *other = mail_reply_find(&drafts, "8:first", true); free(other->text); other->text = xstrdup("other draft");
    mail_reply_finish(d, false, true, accepted, NULL, 90100); CHECK(d->sent); CHECK(!d->open); CHECK_STR(d->text, ""); CHECK_STR(other->text, "other draft");
    mail_reply_finish(d, false, false, NULL, &e, 90100); CHECK(d->sent);
    json_free(accepted); api_error_clear(&e); mail_replies_free(&drafts); CHECK(drafts == NULL);
}

static void refusal_guidance(void) {
    MailReply *drafts = NULL, *d = mail_reply_find(&drafts, "7:first", true);
    free(d->text); d->text = xstrdup("answer");
    ApiError e; api_error_init(&e);
    CHECK(mail_reply_begin(d, 100)); api_error_set(&e, API_HTTP, 502, NULL, -1);
    mail_reply_finish(d, false, false, NULL, &e, 100);
    CHECK(d->uncertain); CHECK(d->error != NULL);
    const char *uncertain_error = d->error;
    CHECK(mail_reply_reconcile(d));
    const int statuses[] = {422, 451};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        CHECK(mail_reply_begin(d, 100)); api_error_set(&e, API_HTTP, statuses[i], NULL, -1);
        mail_reply_finish(d, false, false, NULL, &e, 100);
        CHECK(!d->uncertain); CHECK(!d->sent); CHECK_STR(d->text, "answer");
        CHECK(d->error != NULL); CHECK(!str_eq(d->error, uncertain_error));
        CHECK(!mail_reply_reconcile(d));
    }
    CHECK(mail_reply_begin(d, 100));
    api_error_clear(&e); mail_replies_free(&drafts);
}

static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *bytes, size_t size, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; (void)error; int *calls = ctx; (*calls)++;
    CHECK_STR(method, "POST"); CHECK_STR(url, "https://example.com/api/v1/mail/accounts/7/messages/same%2F%2B%3D/reply");
    char *text = xstrndup(bytes, size); Json *body = json_parsez(text); free(text);
    CHECK_INT(json_count(body), 1); CHECK_STR(json_str(json_get(body, "text")), " Thanks\n"); json_free(body);
    *status = 200; *type = xstrdup("application/json"); *retry = NULL;
    *response = xstrdup("{\"status\":\"accepted\",\"action\":\"reply\"}"); *len = strlen(*response); return true;
}
static void wire(void) {
    ServerAddress address; CHECK(server_address_parse("https://example.com", &address)); ApiError e; api_error_init(&e);
    ApiClient *client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &e); server_address_free(&address);
    int calls = 0; api_client_set_transport(client, transport, &calls);
    Json *args = json_object(); json_set_num(args, "account", 7); json_set_str(args, "id", "same/+="); json_set_str(args, "text", " Thanks\n");
    json_set_str(args, "to", "must-not-forward@example.com"); json_set_bool(args, "replyAll", true);
    Json *answer = api_call(client, "reply_mail_message", args, 0, &e); CHECK(answer != NULL); CHECK_INT(calls, 1); json_free(answer);
    json_set_str(args, "text", " \n"); CHECK(api_call(client, "reply_mail_message", args, 0, &e) == NULL); CHECK_INT(e.status, 400); CHECK_INT(calls, 1);
    json_set_str(args, "text", "valid"); json_set_num(args, "account", 0); CHECK(api_call(client, "reply_mail_message", args, 0, &e) == NULL); CHECK_INT(calls, 1);
    json_free(args); api_error_clear(&e); api_client_release(client);
}
void core_mail_reply_tests(void) {
    test_run("mail replies validate JS Unicode length and preserve exact plain text", validation);
    test_run("mail replies gate writable providers and distinguish account replacement", writable_identity);
    test_run("mail replies accept omitted access and reject explicit malformed access", parsed_access);
    test_run("mail replies preserve drafts, cooldowns and uncertain interlocks", outcomes);
    test_run("mail reply refusals use distinct guidance without reconciliation", refusal_guidance);
    test_run("mail reply wire route encodes original identity and sends text only", wire);
}
