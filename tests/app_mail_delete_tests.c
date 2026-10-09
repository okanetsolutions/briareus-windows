// Isolate confirmation and transport while exercising the inbox mutation handlers.
#include "screens.h"
#include "mail_inbox.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

Screen *delete_test_screen_new(void);
bool delete_test_offered(void);
bool delete_test_current(bool shown, int generation, const Request *r);
void delete_test_thread(Screen *s, const char *thread);
static bool confirmation, revoke_during_confirmation;
static int confirmations, writes;
static Request pending_delete;
static bool confirm_delete(const char *title, const char *message, const char *label, bool destructive) {
    (void)title; (void)message; (void)label; (void)destructive;
    confirmations++;
    if (revoke_during_confirmation) g_store.device.permission = "read";
    return confirmation;
}
static Request *delete_call(const char *op, Json *args, int timeout, void *owner, RequestDone done, int tag, Request **slot) {
    (void)timeout;
    if (!str_eq(op, "delete_mail_message")) { json_free(args); return NULL; }
    writes++; memset(&pending_delete, 0, sizeof pending_delete);
    pending_delete.args = args; pending_delete.operation = xstrdup(op);
    pending_delete.client = g_store.client; pending_delete.owner = owner;
    pending_delete.done = done; pending_delete.tag = tag; pending_delete.slot = slot;
    *slot = &pending_delete;
    return &pending_delete;
}
#define mail_screen_new delete_test_screen_new
#define mail_inbox_offered delete_test_offered
#define mail_inbox_result_current delete_test_current
#define mail_inbox_set_thread_filter delete_test_thread
#define app_confirm confirm_delete
#define store_call delete_call
#include "../app/screen_mail_inbox.c"
#undef mail_screen_new
#undef mail_inbox_offered
#undef mail_inbox_result_current
#undef mail_inbox_set_thread_filter
#undef app_confirm
#undef store_call

static Route DELETE_ROUTES[] = {
    {"GET", "/settings/mail/accounts", "admin"}, {"GET", "/mail/messages", "admin"},
    {"GET", "/mail/accounts/{account}/messages/{id}", "admin"},
    {"DELETE", "/mail/accounts/{account}/messages/{id}", "admin"}
};
static Inbox *delete_setup(void) {
    memset(&g_store, 0, sizeof g_store); memset(&pending_delete, 0, sizeof pending_delete);
    confirmation = true; revoke_during_confirmation = false; confirmations = writes = 0;
    g_store.has_device = g_store.active = true; g_store.device.permission = "admin";
    g_store.routes = DELETE_ROUTES; g_store.route_count = 4;
    ServerAddress address; ApiError error = {0};
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(g_store.client != NULL);
    Inbox *s = (Inbox *)delete_test_screen_new(); s->shown = s->loaded = s->accounts_loaded = true;
    Json *accounts = json_parsez("{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"status\":\"connected\"},{\"id\":8,\"provider\":\"outlook\",\"email\":\"two@example.com\",\"status\":\"connected\"}],\"providers\":[\"gmail\",\"outlook\"]}");
    CHECK(mail_accounts_parse(accounts, &s->accounts)); json_free(accounts);
    Json *page = json_parsez("{\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"Selected\"},{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Other mailbox\"}],\"nextCursor\":\"older\"}");
    CHECK(mail_messages_parse(page, &s->messages)); json_free(page);
    Json *body = json_parsez("{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"Selected\",\"body\":{\"text\":\"Hello\"}}");
    CHECK(mail_message_parse(body, true, &s->body)); json_free(body);
    s->selected_account = 7; s->selected_id = xstrdup("same/+=");
    return s;
}
static void delete_cleanup(Inbox *s) {
    destroy(&s->base); json_free(pending_delete.args); json_free(pending_delete.result);
    free(pending_delete.operation); api_client_release(g_store.client); memset(&g_store, 0, sizeof g_store);
}
static void finish_delete(Inbox *s, bool ok, int status) {
    pending_delete.ok = ok; pending_delete.error.status = status;
    pending_delete.result = json_parsez("{\"ok\":true}");
    s->deletion = NULL; pending_delete.slot = NULL;
    pending_delete.done(pending_delete.owner, &pending_delete);
}
static void delete_requires_confirmation(void) {
    Inbox *s = delete_setup(); confirmation = false;
    action(&s->base, ACT_DELETE, 0, (POINT){0}); CHECK_INT(confirmations, 1); CHECK_INT(writes, 0);
    CHECK_INT(s->messages.count, 2);
    confirmation = true; revoke_during_confirmation = true;
    action(&s->base, ACT_DELETE, 0, (POINT){0}); CHECK_INT(writes, 0);
    g_store.device.permission = "admin"; revoke_during_confirmation = false; g_store.route_count = 3;
    action(&s->base, ACT_DELETE, 0, (POINT){0}); CHECK_INT(writes, 0); CHECK_INT(confirmations, 2);
    delete_cleanup(s);
}
static void delete_selected_only(void) {
    Inbox *s = delete_setup(); action(&s->base, ACT_DELETE, 0, (POINT){0}); CHECK_INT(writes, 1);
    CHECK_INT(json_int_or(json_get(pending_delete.args, "account"), 0), 7);
    CHECK_STR(json_str(json_get(pending_delete.args, "id")), "same/+=");
    action(&s->base, ACT_DELETE, 0, (POINT){0}); CHECK_INT(writes, 1); CHECK_INT(confirmations, 1);
    finish_delete(s, true, 200);
    CHECK_INT(s->messages.count, 1); CHECK_INT(s->messages.messages[0].account_id, 8);
    CHECK_STR(s->messages.next_cursor, "older"); CHECK(s->selected_id == NULL); CHECK(s->body.id == NULL);
    CHECK_STR(s->notice, "Message moved to trash."); delete_cleanup(s);
}
static void delete_failure_preserves_message(void) {
    int statuses[] = {409, 429, 502};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        Inbox *s = delete_setup(); action(&s->base, ACT_DELETE, 0, (POINT){0});
        finish_delete(s, false, statuses[i]); CHECK_INT(s->messages.count, 2); CHECK(s->body.id != NULL);
        CHECK(s->body_error != NULL); CHECK(s->deletion == NULL); delete_cleanup(s);
    }
}
static void deletion_respects_lifecycle(void) {
    Inbox *s = delete_setup();
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 384); layout(&s->base, &doc); doc_end(&doc);
    bool offered = false;
    for (size_t i = 0; i < doc.count; i++) if (str_eq(doc.items[i].text, "Delete message")) {
        offered = true; CHECK_INT(doc.items[i].action, ACT_DELETE);
        CHECK(doc.items[i].rc.left >= 0); CHECK(doc.items[i].rc.right <= 384);
    }
    CHECK(offered); doc_free(&doc);
    action(&s->base, ACT_DELETE, 0, (POINT){0}); s->generation++;
    finish_delete(s, true, 200); CHECK_INT(s->messages.count, 2); CHECK(s->body.id != NULL);
    delete_cleanup(s);
    s = delete_setup(); action(&s->base, ACT_DELETE, 0, (POINT){0});
    visible(&s->base, false); CHECK(pending_delete.cancelled); CHECK(s->deletion == NULL);
    CHECK_INT(s->messages.count, 0); delete_cleanup(s);
}
void app_mail_delete_tests(void) {
    test_run("mail deletion requires confirmation, current admin access and the deployed route", delete_requires_confirmation);
    test_run("mail deletion removes only the confirmed account and message and blocks repeat clicks", delete_selected_only);
    test_run("mail deletion is offered without a web link and discards stale or hidden results", deletion_respects_lifecycle);
    test_run("mail deletion keeps messages after refused or uncertain writes", delete_failure_preserves_message);
}
