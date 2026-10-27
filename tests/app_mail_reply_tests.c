#include "screen_mail_inbox_internal.h"
#include "screen_fixture.h"
#include "modal_driver.h"
#include "resource.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/mail/accounts", "admin"}, {"GET", "/mail/messages", "admin"},
    {"GET", "/mail/accounts/{account}/messages/{id}", "admin"},
    {"POST", "/mail/accounts/{account}/messages/{id}/reply", "admin"},
    {"DELETE", "/mail/accounts/{account}/messages/{id}", "admin"},
};
#define ACCOUNTS "\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"access\":\"manage\",\"status\":\"connected\",\"createdAt\":100},{\"id\":8,\"provider\":\"outlook\",\"email\":\"two@example.com\",\"access\":\"manage\",\"status\":\"connected\",\"createdAt\":200}],\"providers\":[\"gmail\",\"outlook\"]"
#define ROWS "\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First\",\"receivedAt\":200,\"isRead\":true},{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second\",\"receivedAt\":100,\"isRead\":true}]"
static const char *FIRST = "{" ACCOUNTS "," ROWS ",\"message\":{\"accountId\":7,\"id\":\"same/+=\",\"threadId\":\"thread-one\",\"subject\":\"First\",\"from\":{\"address\":\"sender@example.com\"},\"replyTo\":[{\"address\":\"reply@example.com\"}],\"body\":{\"text\":\"First body\"}}}";
static const char *SECOND = "{" ACCOUNTS "," ROWS ",\"message\":{\"accountId\":8,\"id\":\"same/+=\",\"threadId\":\"thread-two\",\"subject\":\"Second\",\"body\":{\"text\":\"Second body\"}}}";

static Inbox *setup(ScreenFixture *f, Doc *doc) {
    screen_fixture_init(f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    f->read_response = FIRST; f->write_response = "{\"status\":\"accepted\",\"action\":\"reply\"}";
    Screen *base = mail_screen_new(); doc_init(doc); base->vt->visible(base, true); screen_fixture_pump(f, 2);
    screen_fixture_select(base, doc, "First"); screen_fixture_pump(f, 1);
    return (Inbox *)base;
}
static void cleanup(Inbox *s, Doc *doc, ScreenFixture *f) {
    s->base.vt->destroy(&s->base); doc_free(doc); screen_fixture_clear(f);
}
static MailReply *draft(Inbox *s, const char *text) {
    MailReply *d = screen_mail_reply_draft(s, true); CHECK(d != NULL);
    if (d) { free(d->text); d->text = xstrdup(text); d->open = true; }
    return d;
}
static void send_reply(Inbox *s) { s->base.vt->action(&s->base, ACT_REPLY_SEND, 0, (POINT){0}); }
static void select_second(Inbox *s, Doc *doc, ScreenFixture *f) {
    f->read_response = SECOND; screen_fixture_select(&s->base, doc, "Second"); screen_fixture_pump(f, 1);
    CHECK_INT(s->body.account_id, 8);
}
static void reopen_first(Inbox *s, Doc *doc, ScreenFixture *f) {
    f->read_response = FIRST; screen_fixture_select(&s->base, doc, "First"); screen_fixture_pump(f, 1);
    CHECK_INT(s->body.account_id, 7);
}
static void gating_and_success(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, " Thanks\n");
    g_store.route_count = 3; send_reply(s); CHECK_INT(f.writes, 0);
    g_store.route_count = 5; g_store.device.permission = "manage"; send_reply(s); CHECK_INT(f.writes, 0);
    g_store.device.permission = "admin"; s->accounts.accounts[0].read_only = true; send_reply(s); CHECK_INT(f.writes, 0);
    s->accounts.accounts[0].read_only = false; free(s->accounts.accounts[0].status); s->accounts.accounts[0].status = xstrdup("reauth");
    send_reply(s); CHECK_INT(f.writes, 0); free(s->accounts.accounts[0].status); s->accounts.accounts[0].status = xstrdup("connected");
    s->accounts.gmail = false; send_reply(s); CHECK_INT(f.writes, 0); s->accounts.gmail = true;
    f.block_write = true; send_reply(s); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    CHECK(d->pending); send_reply(s); CHECK_INT(f.writes, 1);
    CHECK_STR(f.method, "POST"); CHECK_STR(f.url, "https://example.com/api/v1/mail/accounts/7/messages/same%2F%2B%3D/reply");
    CHECK_INT(json_count(f.body), 1); CHECK_STR(json_str(json_get(f.body, "text")), " Thanks\n");
    screen_fixture_draw(&s->base, &doc); int cancel = screen_fixture_find(&doc, "Cancel reply"); CHECK(cancel >= 0);
    if (cancel >= 0) CHECK_INT(doc.items[cancel].action, 0);
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK(!d->pending); CHECK(d->sent); CHECK_STR(d->text, "");
    send_reply(s); CHECK_INT(f.writes, 1); cleanup(s, &doc, &f);
}
static void refused_and_cooldown(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, "keep this");
    f.write_status = 403; f.write_response = "{\"error\":\"private-secret\"}"; send_reply(s); screen_fixture_pump(&f, 1);
    CHECK(!d->pending); CHECK(!d->uncertain); CHECK_STR(d->text, "keep this"); CHECK(d->error != NULL);
    CHECK(s->body.id != NULL); CHECK(s->accounts_loaded); CHECK(!strstr(d->error, "private-secret"));
    f.write_status = 429; f.write_retry = "120"; send_reply(s); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 2); CHECK(d->retry_until >= (double)GetTickCount64() + 110000); send_reply(s); CHECK_INT(f.writes, 2);
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f, 2); reopen_first(s, &doc, &f);
    CHECK(screen_mail_reply_draft(s, false) == d); send_reply(s); CHECK_INT(f.writes, 2);
    s->base.vt->visible(&s->base, false); s->base.vt->visible(&s->base, true); screen_fixture_pump(&f, 2); reopen_first(s, &doc, &f);
    send_reply(s); CHECK_INT(f.writes, 2); CHECK_STR(d->text, "keep this");
    // Advancing the injected deadline proves the interlock, without sleeping for Retry-After.
    d->retry_until = (double)GetTickCount64() - 1; f.write_status = 200; f.write_retry = NULL; f.write_response = "{\"status\":\"accepted\",\"action\":\"reply\"}";
    send_reply(s); screen_fixture_pump(&f, 1); CHECK(d->sent); cleanup(s, &doc, &f);
}
static void independent_drafts_and_cancel(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *first = draft(s, "first draft");
    s->base.vt->action(&s->base, ACT_REPLY_CANCEL, 0, (POINT){0}); CHECK(!first->open); CHECK_STR(first->text, "first draft");
    send_reply(s); CHECK_INT(f.writes, 0); first->open = true;
    select_second(s, &doc, &f); MailReply *second = draft(s, "second draft"); CHECK(first != second);
    send_reply(s); screen_fixture_pump(&f, 1); CHECK(second->sent); CHECK_STR(first->text, "first draft");
    reopen_first(s, &doc, &f); CHECK(screen_mail_reply_draft(s, false) == first);
    f.block_write = true; send_reply(s); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    select_second(s, &doc, &f); CHECK(first->uncertain); CHECK(first->pending); CHECK_STR(first->text, "first draft");
    draft(s, "new second draft"); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK(first->uncertain); CHECK(!first->pending); CHECK_STR(first->text, "first draft"); CHECK_STR(second->text, "new second draft");
    reopen_first(s, &doc, &f); send_reply(s); CHECK_INT(f.writes, 2);
    cleanup(s, &doc, &f);
}
static void uncertain_reopen_reconcile(void) {
    const int statuses[] = {502, 408, 0, 200};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, "uncertain draft");
        f.write_status = statuses[i]; f.fail_write = statuses[i] == 0; f.write_response = "{\"status\":\"queued\"}";
        send_reply(s); screen_fixture_pump(&f, 1); CHECK(d->uncertain); CHECK_STR(d->text, "uncertain draft");
        send_reply(s); CHECK_INT(f.writes, 1);
        s->base.vt->destroy(&s->base); s = (Inbox *)mail_screen_new();
        s->base.vt->visible(&s->base, true); screen_fixture_pump(&f, 2); reopen_first(s, &doc, &f);
        CHECK(screen_mail_reply_draft(s, false) == d); send_reply(s); CHECK_INT(f.writes, 1);
        modal_driver_action(&s->base, ACT_REPLY_RECONCILE, 0, IDCANCEL); screen_fixture_pump(&f, 1); CHECK(d->uncertain);
        modal_driver_action(&s->base, ACT_REPLY_RECONCILE, 0, IDOK); screen_fixture_pump(&f, 1); CHECK(!d->uncertain); CHECK_INT(f.writes, 1);
        f.fail_write = false; f.write_status = 200; f.write_response = "{\"status\":\"accepted\",\"action\":\"reply\"}";
        send_reply(s); screen_fixture_pump(&f, 1); CHECK_INT(f.writes, 2); CHECK(d->sent);
        cleanup(s, &doc, &f);
    }
}
static void stale_read_and_replacement(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, "original draft");
    f.block_read = true; screen_mail_inbox_clear_selection(s); s->selected_id = xstrdup("same/+="); s->selected_account = 7;
    screen_mail_inbox_load_body(s); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    screen_mail_inbox_clear_selection(s); s->selected_id = xstrdup("same/+="); s->selected_account = 8;
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK(s->body.id == NULL); CHECK_STR(d->text, "original draft");
    f.block_read = false; f.read_response = SECOND; screen_mail_inbox_load_body(s); screen_fixture_pump(&f, 1);
    CHECK_INT(s->body.account_id, 8); draft(s, "second");
    char *response = str_replace(SECOND, "two@example.com", "replacement@example.com"); f.read_response = response;
    screen_mail_inbox_load_accounts(s); screen_fixture_pump(&f, 2); CHECK(s->selected_id == NULL); CHECK(s->body.id == NULL);
    send_reply(s); CHECK_INT(f.writes, 0); free(response); f.read_response = FIRST;
    cleanup(s, &doc, &f);
}
static void teardown_and_connection(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, "cancelled draft");
    f.block_write = true; send_reply(s); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base); CHECK(d->uncertain); CHECK(d->pending);
    CHECK(!mail_reply_reconcile(d)); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK(!d->pending); CHECK(d->uncertain); CHECK_STR(d->text, "cancelled draft");
    s = (Inbox *)mail_screen_new(); s->base.vt->visible(&s->base, true); screen_fixture_pump(&f, 2); reopen_first(s, &doc, &f);
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://another.example", &address));
    ApiClient *new_client = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &error); server_address_free(&address); api_error_clear(&error);
    g_store.client = new_client; send_reply(s); CHECK_INT(f.writes, 1); CHECK(screen_mail_reply_draft(s, false) == NULL);
    g_store.client = f.client; api_client_release(new_client); cleanup(s, &doc, &f);
}
static void type_in_draft_dialog(void) {
    HWND dialog = GetActiveWindow();
    CHECK(GetDlgItem(dialog, IDC_INPUT) != NULL);
    wchar_t context[1024] = {0}; GetDlgItemTextW(dialog, IDC_NOTE, context, 1024);
    CHECK(wcsstr(context, L"one@example.com") != NULL);
    CHECK(wcsstr(context, L"reply@example.com") != NULL);
    CHECK(wcsstr(context, L"First") != NULL);
    SetDlgItemTextW(dialog, IDC_INPUT, L"edited\r\nreply");
}
static void draft_dialog_never_sends(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); MailReply *d = draft(s, "original draft");
    modal_driver_action_during(&s->base, ACT_REPLY, 0, IDC_START, type_in_draft_dialog); screen_fixture_pump(&f, 1);
    CHECK_STR(d->text, "edited\nreply"); CHECK_INT(f.writes, 0);
    free(d->text); d->text = xstrdup("previous saved draft");
    modal_driver_action_during(&s->base, ACT_REPLY, 0, IDCANCEL, type_in_draft_dialog); screen_fixture_pump(&f, 1);
    CHECK_STR(d->text, "previous saved draft"); CHECK_INT(f.writes, 0);
    cleanup(s, &doc, &f);
}
static void old_completion_cannot_touch_new_connection(void) {
    ScreenFixture f; Doc doc; Inbox *s = setup(&f, &doc); draft(s, "old connection");
    f.block_write = true; send_reply(s); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base);
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://replacement.example", &address));
    ApiClient *replacement = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &error);
    server_address_free(&address); api_error_clear(&error);
    mail_replies_free(&g_store.mail_replies); g_store.client = replacement;
    MailReply *d = mail_reply_find(&g_store.mail_replies, "unrelated", true);
    free(d->text); d->text = xstrdup("new connection draft");
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK_STR(d->text, "new connection draft"); CHECK(!d->sent); CHECK(!d->uncertain);
    g_store.client = f.client; api_client_release(replacement); doc_free(&doc); screen_fixture_clear(&f);
}
void app_mail_reply_tests(void) {
    screen_fixture_prepare();
    test_run("mail reply dialog saves plain text and Cancel preserves previous draft without sending", draft_dialog_never_sends);
    test_run("mail reply old completion cannot mutate replacement connection draft", old_completion_cannot_touch_new_connection);
    test_run("mail reply requires route admin and current writable provider; exact send blocks duplicates", gating_and_success);
    test_run("mail reply refusal retains draft and Retry-After survives refresh and hide", refused_and_cooldown);
    test_run("mail reply navigation preserves independent drafts and cancels ownership", independent_drafts_and_cancel);
    test_run("mail reply uncertain outcomes survive destruction until explicit Sent reconciliation", uncertain_reopen_reconcile);
    test_run("mail reply ignores stale body and invalidates reused account identity", stale_read_and_replacement);
    test_run("mail reply teardown keeps uncertain interlock and refuses another connection", teardown_and_connection);
}
