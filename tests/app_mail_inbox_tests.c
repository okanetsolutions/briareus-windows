// Automated screen/transport regressions; no provider calls, sign-in or mailbox mutations.
#include "screens.h"
#include "mail_inbox.h"
#include "mail.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

static Route ROUTES[] = {
    {"GET", "/settings/mail/accounts", "admin"}, {"GET", "/mail/messages", "admin"},
    {"GET", "/mail/accounts/{account}/messages/{id}", "admin"}
};
static const char *ACCOUNTS = "{\"accounts\":[{\"id\":7,\"email\":\"one@example.com\",\"provider\":\"gmail\",\"status\":\"connected\"},{\"id\":8,\"email\":\"two@example.com\",\"provider\":\"outlook\",\"status\":\"connected\"}],\"providers\":[\"gmail\",\"outlook\"]}";
static const char *PAGE = "{\"messages\":[{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second account\",\"receivedAt\":200,\"isRead\":false},{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First account\",\"receivedAt\":100,\"isRead\":false}],\"nextCursor\":\"older/+=\"}";
static const char *OLDER = "{\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First account\",\"receivedAt\":100},{\"accountId\":7,\"id\":\"old\",\"subject\":\"Older\",\"receivedAt\":50}],\"nextCursor\":null}";
typedef struct {
    const char *accounts, *page;
    int body_status, list_status, account_status;
    double retry_after;
    volatile LONG calls, list_calls, body_calls;
    char *last_list, *last_body;
    HANDLE entered, resume;
    bool block_first, block_body;
} InboxStub;
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)body; (void)timeout;
    InboxStub *s = ctx;
    // Capture on the worker; assert only after completion has been delivered on the UI thread.
    bool read = str_eq(method, "GET") && bytes == 0;
    const char *answer = "{}"; *status = read ? 200 : 500; *error = NULL;
    if (strstr(url, "settings/mail/accounts")) {
        answer = s->accounts;
        if (s->account_status) { *status = s->account_status; answer = "{}"; }
    }
    else if (strstr(url, "/mail/messages")) {
        LONG n = InterlockedIncrement(&s->list_calls);
        free(s->last_list); s->last_list = xstrdup(url);
        if (s->block_first && n == 1) { SetEvent(s->entered); WaitForSingleObject(s->resume, 5000); }
        answer = strstr(url, "cursor=") ? OLDER : s->page ? s->page : PAGE;
        if (strstr(url, "account=7")) answer = "{\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First account\",\"receivedAt\":100}],\"nextCursor\":null}";
        if (strstr(url, "account=8")) answer = "{\"messages\":[{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second account\",\"receivedAt\":200}],\"nextCursor\":null}";
        if (s->list_status) { *status = s->list_status; answer = "{\"error\":\"private provider error\"}"; }
    } else {
        LONG n = InterlockedIncrement(&s->body_calls); free(s->last_body); s->last_body = xstrdup(url);
        if (s->block_body && n == 1) { SetEvent(s->entered); WaitForSingleObject(s->resume, 5000); }
        bool second = strstr(url, "/8/") != NULL;
        answer = second ? "{\"message\":{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second account\",\"isRead\":false,\"body\":{\"text\":\"<script>literal</script> [link](https://remote)\",\"html\":\"<script>active</script>\",\"truncated\":true},\"attachments\":[{\"name\":\"report.pdf\",\"size\":123,\"mimeType\":\"application/pdf\"}]}}"
            : "{\"message\":{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First account\",\"isRead\":false,\"body\":{\"text\":\"First body\"}}}";
        if (s->body_status) { *status = s->body_status; answer = "{\"error\":\"private provider error\"}"; }
    }
    *type = xstrdup("application/json"); *retry = s->retry_after > 0 ? xstrfmt("%.0f", s->retry_after) : NULL;
    *response = xstrdup(answer); *len = strlen(*response); InterlockedIncrement(&s->calls); return true;
}
static ApiClient *setup(InboxStub *stub) {
    memset(&g_store, 0, sizeof g_store); g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = ROUTES; g_store.route_count = 3; g_store.active = true;
    // Successful worker replies need a UI-thread window; NULL posts onto the worker's own queue.
    g_store.hwnd = CreateWindowExW(0, L"STATIC", L"Mail regression dispatcher", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(g_store.hwnd != NULL);
    ServerAddress address; ApiError e; api_error_init(&e);
    CHECK(server_address_parse("https://example.com", &address));
    g_store.client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &e);
    CHECK(g_store.client != NULL); server_address_free(&address); api_error_clear(&e);
    api_client_set_transport(g_store.client, transport, stub);
    return g_store.client;
}
static void pump(int expected) {
    ULONGLONG end = GetTickCount64() + 10000; int delivered = 0;
    while (delivered < expected && GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, g_store.hwnd, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); delivered++;
        }
        if (delivered < expected) Sleep(1);
    }
    CHECK_INT(delivered, expected);
}
static void draw(Screen *s, Doc *doc) {
    doc_begin(doc, NULL, 800); s->vt->layout(s, doc); doc_end(doc);
}
static int find_text(Doc *doc, const char *text) {
    for (size_t i = 0; i < doc->count; i++) if (str_eq(doc->items[i].text, text)) return (int)i;
    return -1;
}
static void click(Screen *s, Doc *doc, const char *text) {
    draw(s, doc); int i = find_text(doc, text); CHECK(i >= 0);
    if (i >= 0) { CHECK(doc->items[i].action != 0); s->vt->action(s, doc->items[i].action, doc->items[i].arg, (POINT){0}); }
}
static void select_subject(Screen *s, Doc *doc, const char *subject) {
    draw(s, doc); int i = find_text(doc, subject); CHECK(i > 0);
    if (i > 0) { Item *box = &doc->items[i - 1]; CHECK(box->action != 0); s->vt->action(s, box->action, box->arg, (POINT){0}); }
}
static void cleanup(Screen *s, Doc *doc, ApiClient *client, InboxStub *stub) {
    s->vt->destroy(s); doc_free(doc); api_client_release(client); free(stub->last_list); free(stub->last_body);
    if (stub->entered) CloseHandle(stub->entered);
    if (stub->resume) CloseHandle(stub->resume);
    DestroyWindow(g_store.hwnd); memset(&g_store, 0, sizeof g_store);
}
static void screen_reads(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; ApiClient *client = setup(&stub);
    Screen *s = mail_screen_new(); Doc doc; doc_init(&doc); s->vt->visible(s, true); pump(2);
    CHECK_INT(stub.body_calls, 0); draw(s, &doc);
    CHECK(find_text(&doc, "Second account") < find_text(&doc, "First account"));
    select_subject(s, &doc, "Second account"); pump(1);
    CHECK_STR(stub.last_body, "https://example.com/api/v1/mail/accounts/8/messages/same%2F%2B%3D");
    draw(s, &doc); int i = find_text(&doc, "<script>literal</script> [link](https://remote)"); CHECK(i >= 0);
    if (i >= 0) CHECK_INT(doc.items[i].action, 0);
    CHECK(find_text(&doc, "<script>active</script>") == -1);
    CHECK(find_text(&doc, "report.pdf | application/pdf | 123 bytes") >= 0);
    CHECK(find_text(&doc, "The server truncated this body. Open at the provider to read the complete message.") >= 0);
    click(s, &doc, "Close message"); select_subject(s, &doc, "First account"); pump(1);
    CHECK_STR(stub.last_body, "https://example.com/api/v1/mail/accounts/7/messages/same%2F%2B%3D");
    click(s, &doc, "Close message"); click(s, &doc, "Load older messages"); pump(1);
    CHECK(strstr(stub.last_list, "cursor=older%2F%2B%3D") != NULL); draw(s, &doc);
    CHECK(find_text(&doc, "Older") >= 0); CHECK(find_text(&doc, "Load older messages") == -1);
    int duplicates = 0; for (size_t k = 0; k < doc.count; k++) if (str_eq(doc.items[k].text, "First account")) duplicates++;
    CHECK_INT(duplicates, 1);
    // Unread and starred are toggles. Folder false is the Outside inbox item. Each change drops the cursor.
    click(s, &doc, "Unread only"); pump(2);
    CHECK(strstr(stub.last_list, "unread=1") != NULL); CHECK(strstr(stub.last_list, "cursor=") == NULL);
    click(s, &doc, "Unread only"); pump(2); CHECK(strstr(stub.last_list, "unread=") == NULL);
    click(s, &doc, "Starred"); pump(2);
    CHECK(strstr(stub.last_list, "starred=1") != NULL); CHECK(strstr(stub.last_list, "cursor=") == NULL);
    click(s, &doc, "Folder"); click(s, &doc, "Outside inbox"); pump(2);
    CHECK(strstr(stub.last_list, "inbox=0") != NULL); CHECK(strstr(stub.last_list, "cursor=") == NULL);
    click(s, &doc, "Reset filters"); pump(2); CHECK(strstr(stub.last_list, "unread=") == NULL);
    click(s, &doc, "Mailbox"); click(s, &doc, "one@example.com"); pump(2); draw(s, &doc);
    CHECK(strstr(stub.last_list, "account=7") != NULL); CHECK(strstr(stub.last_list, "cursor=") == NULL);
    CHECK(find_text(&doc, "First account") >= 0); CHECK(find_text(&doc, "Second account") == -1);
    click(s, &doc, "Mailbox"); click(s, &doc, "All mailboxes"); pump(2); CHECK(strstr(stub.last_list, "account=") == NULL);
    s->vt->visible(s, false); draw(s, &doc); CHECK(find_text(&doc, "First account") == -1); CHECK(find_text(&doc, "Older") == -1);
    cleanup(s, &doc, client, &stub);
}
static void stale_page(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; stub.block_first = true;
    stub.entered = CreateEventW(NULL, TRUE, FALSE, NULL); stub.resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    s->vt->visible(s, true); pump(1); CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    // A new filter generation cancels the first page while it is in flight.
    click(s, &doc, "Reset filters"); pump(2); SetEvent(stub.resume); pump(1);
    draw(s, &doc); int titles = 0;
    for (size_t i = 0; i < doc.count; i++) if (str_eq(doc.items[i].text, "First account")) titles++;
    CHECK_INT(titles, 1); CHECK_INT(stub.list_calls, 2);
    // Account removal/revocation purges the old messages and selected body before reloading.
    select_subject(s, &doc, "Second account"); pump(1);
    stub.accounts = "{\"accounts\":[{\"id\":7,\"email\":\"one@example.com\",\"provider\":\"gmail\",\"status\":\"reauth\"}],\"providers\":[\"gmail\"]}";
    s->vt->timer(s, 1910); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Second account") == -1); CHECK(find_text(&doc, "First account") == -1);
    CHECK(find_text(&doc, "<script>literal</script> [link](https://remote)") == -1);
    cleanup(s, &doc, client, &stub);
}
static void failures(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; ApiClient *client = setup(&stub);
    Screen *s = mail_screen_new(); Doc doc; doc_init(&doc); s->vt->visible(s, true); pump(2);
    stub.body_status = 404; select_subject(s, &doc, "Second account"); pump(3); draw(s, &doc);
    CHECK(find_text(&doc, "Close message") == -1); CHECK(find_text(&doc, "First account") >= 0);
    CHECK(find_text(&doc, "This message or account was removed from the synced copy. Refresh the list.") >= 0);
    stub.body_status = 429; stub.retry_after = 90; select_subject(s, &doc, "Second account"); pump(1); draw(s, &doc);
    CHECK(find_text(&doc, "Mail cooldown is active. Retry buttons become available when it ends.") >= 0);
    int i = find_text(&doc, "Retry message"); CHECK(i >= 0); if (i >= 0) CHECK_INT(doc.items[i].action, 0);
    int calls = (int)stub.calls; s->vt->refresh(s); s->vt->timer(s, 1910); CHECK_INT(stub.calls, calls);
    s->vt->visible(s, false); s->vt->visible(s, true); CHECK_INT(stub.calls, calls); // Screen changes cannot bypass Retry-After.
    ULONGLONG until = g_store.mail_retry_until;
    s->vt->destroy(s); s = mail_screen_new(); s->vt->visible(s, true);
    s->vt->refresh(s); s->vt->timer(s, 1910); draw(s, &doc);
    CHECK_INT(stub.calls, calls); CHECK(g_store.mail_retry_until == until); CHECK_INT(g_store.mail_failures, 1);
    CHECK(find_text(&doc, "Mail cooldown is active. Retry buttons become available when it ends.") >= 0);
    // Simulate the deadline passing without waiting for the server's 90-second cooldown.
    g_store.mail_retry_until = GetTickCount64(); s->vt->timer(s, 1910); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "First account") >= 0); CHECK_INT(g_store.mail_failures, 0);
    cleanup(s, &doc, client, &stub);
}
static void readable_accounts(void) {
    const char *initial[] = {
        "{\"accounts\":[{\"id\":7,\"email\":\"one@example.com\",\"provider\":\"gmail\",\"status\":\"connected\"},{\"id\":8,\"email\":\"two@example.com\",\"provider\":\"outlook\",\"status\":\"reauth\"}],\"providers\":[\"gmail\",\"outlook\"]}",
        "{\"accounts\":[{\"id\":7,\"email\":\"one@example.com\",\"provider\":\"gmail\",\"status\":\"connected\"}],\"providers\":[\"gmail\"]}"
    };
    for (size_t i = 0; i < sizeof initial / sizeof *initial; i++) {
        InboxStub stub = {0}; stub.accounts = initial[i];
        if (i == 1) stub.page = "{\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"First account\",\"receivedAt\":100}],\"nextCursor\":\"older/+=\"}";
        ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
        s->vt->visible(s, true); pump(2); draw(s, &doc);
        CHECK(find_text(&doc, "First account") >= 0); CHECK(find_text(&doc, "Second account") == -1);
        click(s, &doc, "Load older messages"); pump(1); draw(s, &doc);
        CHECK(find_text(&doc, "Older") >= 0); CHECK(find_text(&doc, "Load older messages") == -1);
        s->vt->timer(s, 1910); pump(1); CHECK_INT(stub.list_calls, 2); // Unchanged accounts keep loaded pages.
        stub.accounts = ACCOUNTS; stub.page = PAGE;
        s->vt->timer(s, 1910); pump(2); draw(s, &doc);
        CHECK_INT(stub.list_calls, 3); CHECK(strstr(stub.last_list, "cursor=") == NULL);
        CHECK(find_text(&doc, "Second account") >= 0); CHECK(find_text(&doc, "First account") >= 0);
        CHECK(find_text(&doc, "Older") == -1); CHECK(find_text(&doc, "Load older messages") >= 0);
        cleanup(s, &doc, client, &stub);
    }
}
static void pruned_pages(void) {
    const char *pages[] = {
        "{\"messages\":[{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second account\",\"receivedAt\":200}],\"nextCursor\":\"older/+=\"}",
        "{\"messages\":[{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Second account\",\"receivedAt\":200}],\"nextCursor\":null}"
    };
    for (size_t i = 0; i < sizeof pages / sizeof *pages; i++) {
        InboxStub stub = {0}; stub.page = pages[i];
        stub.accounts = "{\"accounts\":[{\"id\":7,\"email\":\"one@example.com\",\"provider\":\"gmail\",\"status\":\"connected\"},{\"id\":8,\"email\":\"two@example.com\",\"provider\":\"outlook\",\"status\":\"reauth\"}],\"providers\":[\"gmail\",\"outlook\"]}";
        ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
        s->vt->visible(s, true); pump(2); draw(s, &doc);
        CHECK(find_text(&doc, "Second account") == -1); CHECK_INT(stub.list_calls, 1);
        CHECK((find_text(&doc, "No synced messages match these filters.") >= 0) == (i == 1));
        CHECK((find_text(&doc, "No readable messages on the loaded pages. Load older messages to keep looking.") >= 0) == (i == 0));
        CHECK((find_text(&doc, "Load older messages") >= 0) == (i == 0));
        if (i == 0) {
            click(s, &doc, "Load older messages"); pump(1); draw(s, &doc);
            CHECK(strstr(stub.last_list, "cursor=older%2F%2B%3D") != NULL);
            CHECK(find_text(&doc, "Older") >= 0); CHECK(find_text(&doc, "Load older messages") == -1);
            CHECK(find_text(&doc, "No readable messages on the loaded pages. Load older messages to keep looking.") == -1);
        }
        cleanup(s, &doc, client, &stub);
    }
}
static void account_backoff(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; ApiClient *client = setup(&stub);
    Screen *s = mail_screen_new(); Doc doc; doc_init(&doc); s->vt->visible(s, true); pump(2);
    stub.account_status = 503;
    for (int i = 1; i <= 2; i++) {
        g_store.mail_retry_until = GetTickCount64(); s->vt->timer(s, 1910); pump(1);
        CHECK_INT(g_store.mail_failures, i);
    }
    stub.account_status = 0; g_store.mail_retry_until = GetTickCount64(); ULONGLONG until = g_store.mail_retry_until;
    s->vt->timer(s, 1910); pump(1);
    CHECK_INT(stub.list_calls, 1); CHECK_INT(g_store.mail_failures, 0); CHECK(g_store.mail_retry_until == until);
    stub.account_status = 503; s->vt->timer(s, 1910); pump(1);
    CHECK_INT(g_store.mail_failures, 1);
    CHECK(g_store.mail_retry_until > GetTickCount64()); CHECK(g_store.mail_retry_until <= GetTickCount64() + 10000);
    cleanup(s, &doc, client, &stub);
}
static void first_page_backoff(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; stub.list_status = 503;
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    const int delays[] = { 10000, 20000, 40000, 60000, 60000 };
    for (int i = 0; i < 5; i++) {
        // Expire the cooldown without waiting; each retry first reads accounts successfully.
        g_store.mail_retry_until = GetTickCount64(); ULONGLONG before = GetTickCount64();
        if (i == 0) s->vt->visible(s, true); else s->vt->timer(s, 1910);
        pump(2);
        CHECK_INT(stub.list_calls, i + 1); CHECK_INT(stub.calls, 2 * (i + 1));
        CHECK_INT(g_store.mail_failures, i < 4 ? i + 1 : 4);
        CHECK(g_store.mail_retry_until >= before + (ULONGLONG)delays[i]);
        CHECK(g_store.mail_retry_until <= GetTickCount64() + (ULONGLONG)delays[i]);
        s->vt->timer(s, 1910); CHECK_INT(stub.calls, 2 * (i + 1));
    }
    stub.list_status = 0; g_store.mail_retry_until = GetTickCount64(); ULONGLONG until = g_store.mail_retry_until;
    s->vt->timer(s, 1910); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "First account") >= 0); CHECK_INT(stub.list_calls, 6);
    CHECK_INT(g_store.mail_failures, 0); CHECK(g_store.mail_retry_until == until);
    cleanup(s, &doc, client, &stub);
}
static void body_backoff(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; ApiClient *client = setup(&stub);
    Screen *s = mail_screen_new(); Doc doc; doc_init(&doc); s->vt->visible(s, true); pump(2);
    stub.body_status = 503; select_subject(s, &doc, "First account"); pump(1); CHECK_INT(g_store.mail_failures, 1);
    g_store.mail_retry_until = GetTickCount64(); click(s, &doc, "Retry message"); pump(1); CHECK_INT(g_store.mail_failures, 2);
    stub.body_status = 0; g_store.mail_retry_until = GetTickCount64(); ULONGLONG until = g_store.mail_retry_until;
    click(s, &doc, "Retry message"); pump(1); draw(s, &doc);
    CHECK(find_text(&doc, "First body") >= 0); CHECK_INT(stub.list_calls, 1); CHECK_INT(stub.calls, 5);
    CHECK_INT(g_store.mail_failures, 0); CHECK(g_store.mail_retry_until == until);
    stub.body_status = 503; select_subject(s, &doc, "Second account"); pump(1);
    CHECK_INT(g_store.mail_failures, 1);
    CHECK(g_store.mail_retry_until > GetTickCount64()); CHECK(g_store.mail_retry_until <= GetTickCount64() + 10000);
    cleanup(s, &doc, client, &stub);
}
static void stale_body(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; stub.block_body = true;
    stub.entered = CreateEventW(NULL, TRUE, FALSE, NULL); stub.resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    s->vt->visible(s, true); pump(2); select_subject(s, &doc, "Second account");
    CHECK_INT(WaitForSingleObject(stub.entered, 10000), WAIT_OBJECT_0);
    select_subject(s, &doc, "First account"); pump(1); SetEvent(stub.resume); pump(1); draw(s, &doc);
    CHECK(find_text(&doc, "First body") >= 0); CHECK(find_text(&doc, "<script>literal</script> [link](https://remote)") == -1);
    // A route disappearing at the account status refresh also clears private content.
    stub.account_status = 404; s->vt->timer(s, 1910); pump(1); draw(s, &doc);
    CHECK(find_text(&doc, "First body") == -1); CHECK(find_text(&doc, "First account") == -1);
    stub.account_status = 0; click(s, &doc, "Retry account status"); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "First account") >= 0);
    stub.body_status = 403; select_subject(s, &doc, "First account"); pump(1); draw(s, &doc);
    CHECK(find_text(&doc, "First account") == -1); CHECK(find_text(&doc, "First body") == -1);
    cleanup(s, &doc, client, &stub);
}
static void list_failures(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; stub.list_status = 404;
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    s->vt->visible(s, true); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Retry account status") >= 0); CHECK(find_text(&doc, "First account") == -1);
    stub.list_status = 0; click(s, &doc, "Retry account status"); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "First account") >= 0);
    stub.list_status = 503; stub.retry_after = 90; s->vt->refresh(s); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Mail is unavailable on the server. Retry after the cooldown.") >= 0);
    int i = find_text(&doc, "Retry list"); CHECK(i >= 0); if (i >= 0) CHECK_INT(doc.items[i].action, 0);
    CHECK(find_text(&doc, "private provider error") == -1);
    cleanup(s, &doc, client, &stub);
}
static void people_and_notifications(void) {
    InboxStub stub = {0};
    stub.accounts = ACCOUNTS;
    stub.page = "{\"messages\":["
        "{\"accountId\":7,\"id\":\"p\",\"subject\":\"From a person\",\"receivedAt\":300,\"sender\":\"Ada <ada@example.com>\"},"
        "{\"accountId\":7,\"id\":\"n\",\"subject\":\"From a bot\",\"receivedAt\":200,\"sender\":\"Bot <noreply@example.com>\"},"
        "{\"accountId\":7,\"id\":\"c\",\"subject\":\"From a list\",\"receivedAt\":100,\"sender\":\"News <news@example.com>\",\"labels\":[\"CATEGORY_PROMOTIONS\"]},"
        "{\"accountId\":7,\"id\":\"e\",\"subject\":\"No sender\",\"receivedAt\":50}"
        "],\"nextCursor\":null}";
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    s->vt->visible(s, true); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "From a person") >= 0); CHECK(find_text(&doc, "No sender") >= 0);
    CHECK(find_text(&doc, "From a bot") == -1); CHECK(find_text(&doc, "From a list") == -1);
    click(s, &doc, "Notifications"); draw(s, &doc);
    CHECK(find_text(&doc, "From a person") == -1); CHECK(find_text(&doc, "No sender") == -1);
    CHECK(find_text(&doc, "From a bot") >= 0); CHECK(find_text(&doc, "From a list") >= 0);
    s->vt->refresh(s); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "From a bot") >= 0); CHECK(find_text(&doc, "From a person") == -1);
    click(s, &doc, "All"); draw(s, &doc);
    CHECK(find_text(&doc, "From a person") >= 0); CHECK(find_text(&doc, "From a bot") >= 0); CHECK(find_text(&doc, "From a list") >= 0);
    click(s, &doc, "People"); draw(s, &doc);
    CHECK(find_text(&doc, "From a bot") == -1); CHECK(find_text(&doc, "From a person") >= 0);
    cleanup(s, &doc, client, &stub);
}
static void thread_grouping(void) {
    InboxStub stub = {0};
    stub.accounts = ACCOUNTS;
    stub.page = "{\"messages\":["
        "{\"accountId\":7,\"id\":\"a\",\"threadId\":\"t1\",\"subject\":\"Thread head\",\"receivedAt\":300,\"sender\":\"Ada <ada@example.com>\"},"
        "{\"accountId\":7,\"id\":\"b\",\"threadId\":\"t1\",\"subject\":\"Thread reply\",\"receivedAt\":200,\"sender\":\"Bea <bea@example.com>\"},"
        "{\"accountId\":8,\"id\":\"c\",\"threadId\":\"t1\",\"subject\":\"Other mailbox\",\"receivedAt\":100,\"sender\":\"Cy <cy@example.com>\"}"
        "],\"nextCursor\":null}";
    ApiClient *client = setup(&stub); Screen *s = mail_screen_new(); Doc doc; doc_init(&doc);
    s->vt->visible(s, true); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Thread head") >= 0); CHECK(find_text(&doc, "Thread reply") == -1);
    CHECK(find_text(&doc, "Other mailbox") >= 0); CHECK(find_text(&doc, "2 in thread") >= 0);
    s->vt->refresh(s); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Thread reply") == -1); CHECK(find_text(&doc, "2 in thread") >= 0);
    click(s, &doc, "Grouping"); click(s, &doc, "Separate messages"); draw(s, &doc);
    CHECK(find_text(&doc, "Thread head") >= 0); CHECK(find_text(&doc, "Thread reply") >= 0);
    CHECK(find_text(&doc, "Other mailbox") >= 0); CHECK(find_text(&doc, "2 in thread") == -1);
    click(s, &doc, "Reset filters"); pump(2); draw(s, &doc);
    CHECK(find_text(&doc, "Thread reply") == -1); CHECK(find_text(&doc, "2 in thread") >= 0);
    cleanup(s, &doc, client, &stub);
}
static void permissions(void) {
    InboxStub stub = {0}; stub.accounts = ACCOUNTS; ApiClient *client = setup(&stub);
    const char *permission[] = { "read", "manage", "unknown", "admin" };
    for (size_t i = 0; i < sizeof permission / sizeof *permission; i++) {
        g_store.device.permission = (char *)permission[i]; CHECK(mail_inbox_offered() == (i == 3));
    }
    Request r = {0}; r.client = client; r.tag = 4; r.operation = "mail_messages";
    CHECK(mail_inbox_result_current(true, 4, &r)); CHECK(!mail_inbox_result_current(true, 5, &r));
    CHECK(!mail_inbox_result_current(false, 4, &r)); r.cancelled = true; CHECK(!mail_inbox_result_current(true, 4, &r)); r.cancelled = false;
    g_store.client = NULL; CHECK(!mail_inbox_result_current(true, 4, &r)); g_store.client = client;
    g_store.route_count = 0; CHECK(!mail_inbox_offered()); g_store.route_count = 1; CHECK(!mail_inbox_offered());
    g_store.route_count = 2; CHECK(mail_inbox_offered()); CHECK(!store_supports("mail_message"));
    g_store.route_count = 3; Screen *s = mail_screen_new(); Doc doc; doc_init(&doc); s->vt->visible(s, true); pump(2);
    g_store.device.permission = "manage"; draw(s, &doc); CHECK(find_text(&doc, "First account") == -1);
    cleanup(s, &doc, client, &stub);
}
void app_mail_inbox_tests(void) {
    if (!theme.canvas) theme_init();
    test_run("mail inbox reads selected account bodies as literal text and resets pagination on filters", screen_reads);
    test_run("mail inbox splits people from notifications without refetching", people_and_notifications);
    test_run("mail inbox groups one thread per mailbox until messages are separated", thread_grouping);
    test_run("mail inbox ignores superseded pages and clears removed or revoked accounts", stale_page);
    test_run("mail missing messages recover and cooldown survives screen changes", failures);
    test_run("mail inbox reloads loaded pages when accounts reconnect or are added", readable_accounts);
    test_run("mail inbox distinguishes pruned pages with older mail from exhausted empty results", pruned_pages);
    test_run("mail account reads reset backoff without reloading messages", account_backoff);
    test_run("mail first-page failures escalate backoff despite successful account polls", first_page_backoff);
    test_run("mail body reads reset backoff without an account or list read", body_backoff);
    test_run("mail selection rejects stale bodies and clears private content on route or permission failures", stale_body);
    test_run("mail missing list routes recover and unavailable lists respect Retry-After", list_failures);
    test_run("mail inbox and bodies require catalog admin permission and current generations", permissions);
}
