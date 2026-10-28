#include "mail_settings.h"
#include "screens.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "screen_fixture.h"
#include "modal_driver.h"
#include "screen_mail_internal.h"
#include <limits.h>
#include <math.h>
#include <string.h>

static Route ROUTES[] = {
    { "GET", "/settings/mail/accounts", "admin" },
    { "POST", "/settings/mail/accounts/connect", "admin" },
    { "POST", "/settings/mail/accounts/connect/finish", "admin" },
    { "PUT", "/settings/mail/accounts/{id}", "admin" },
    { "DELETE", "/settings/mail/accounts/{id}", "admin" },
    { "POST", "/settings/mail/accounts/{id}/sync", "admin" },
};
static void setup(const char *permission, size_t routes) {
    memset(&g_store, 0, sizeof g_store); g_store.has_device = true;
    g_store.device.permission = (char *)permission; g_store.routes = ROUTES; g_store.route_count = routes;
}
static void permissions(void) {
    const char *ops[] = { "settings_mail_accounts", "connect_mail_account", "finish_mail_account", "update_mail_account", "delete_mail_account", "sync_mail_account" };
    const char *permissions[] = { "read", "manage", "unknown", "admin" };
    for (size_t p = 0; p < sizeof permissions / sizeof *permissions; p++) {
        setup(permissions[p], 6); CHECK(mail_settings_offered() == (p == 3));
        for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) CHECK(store_supports(ops[i]) == (p == 3));
    }
    setup("admin", 0); CHECK(!mail_settings_offered()); // Older server.
    setup("admin", 1); CHECK(mail_settings_offered()); CHECK(!store_supports("connect_mail_account"));
    CHECK(!store_supports("finish_mail_account")); CHECK(!store_supports("update_mail_account"));
    CHECK(!store_supports("delete_mail_account")); CHECK(!store_supports("sync_mail_account"));
    memset(&g_store, 0, sizeof g_store);
}
static void lifecycle(void) {
    setup("admin", 6);
    ApiError e; api_error_init(&e); ServerAddress address;
    CHECK(server_address_parse("https://example.com", &address));
    ApiClient *one = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &e);
    ApiClient *two = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &e);
    CHECK(one && two); server_address_free(&address);
    g_store.client = one; Request r; memset(&r, 0, sizeof r); r.client = one;
    CHECK(mail_settings_result_current(true, &r));
    r.cancelled = true; CHECK(!mail_settings_result_current(true, &r)); r.cancelled = false;
    CHECK(!mail_settings_result_current(false, &r)); // Screen left during sign-in/sync.
    g_store.client = two; CHECK(!mail_settings_result_current(true, &r)); // Pairing/server/account changed.
    g_store.client = one; g_store.device.permission = "manage"; CHECK(!mail_settings_result_current(true, &r));
    g_store.device.permission = "admin"; g_store.route_count = 0; CHECK(!mail_settings_result_current(true, &r));
    r.client = NULL; CHECK(!mail_settings_result_current(true, &r));
    // Cancelled exchange results never call a retired screen or clear its replacement slot.
    Request cancelled; memset(&cancelled, 0, sizeof cancelled); Request *slot = &cancelled;
    request_cancel(&slot); CHECK(cancelled.cancelled && !slot && !cancelled.slot);
    api_client_release(one); api_client_release(two); api_error_clear(&e); memset(&g_store, 0, sizeof g_store);
}
typedef struct { int calls, tag; } MailAnswer;
static void answered(void *owner, Request *r) { MailAnswer *answer = owner; answer->calls++; answer->tag = r->tag; }
static void cancellation(void) {
    setup("admin", 6); // No connection: refusals are posted to this thread, exercising real delivery/cancellation.
    MailAnswer retired = {0}, replacement = {0}; Request *old_slot = NULL, *new_slot = NULL;
    store_call("settings_mail_accounts", json_object(), 1000, &retired, answered, 7, &old_slot);
    store_call("finish_mail_account", json_object(), 1000, &retired, answered, 7, &old_slot);
    request_cancel(&old_slot); CHECK(old_slot == NULL);
    store_call("sync_mail_account", json_object(), 1000, &replacement, answered, 8, &new_slot);
    MSG msg; int delivered = 0;
    while (PeekMessageW(&msg, (HWND)-1, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
        store_handle_message(msg.message, msg.wParam, msg.lParam); delivered++;
    }
    CHECK_INT(delivered, 3); CHECK_INT(retired.calls, 0); CHECK_INT(replacement.calls, 1);
    CHECK_INT(replacement.tag, 8); CHECK(new_slot == NULL);
    memset(&g_store, 0, sizeof g_store);
}
static void backoff(void) {
    CHECK_INT(mail_settings_retry_ms(0, -1), 10000); CHECK_INT(mail_settings_retry_ms(1, -1), 10000);
    CHECK_INT(mail_settings_retry_ms(2, -1), 20000); CHECK_INT(mail_settings_retry_ms(3, -1), 40000);
    CHECK_INT(mail_settings_retry_ms(4, -1), 60000); CHECK_INT(mail_settings_retry_ms(100000, -1), 60000);
    CHECK_INT(mail_settings_retry_ms(1, 90.5), 90500); CHECK_INT(mail_settings_retry_ms(1, 0.1), 10000);
    CHECK_INT(mail_settings_retry_ms(1, NAN), 10000); CHECK_INT(mail_settings_retry_ms(1, 1e100), INT_MAX);
}
static MailScreen *disconnecting;
static void nested_close(void) {
    CHECK(!pane_can_leave(disconnecting->base.pane));
    CHECK(disconnecting->modal);
    CHECK(IsWindow(disconnecting->label_edit));
}
static void nested_close_then_retire(void) {
    nested_close();
    pane_set_root(disconnecting->base.pane, NULL);
    CHECK(disconnecting->modal); CHECK(disconnecting->retired);
    CHECK(IsWindow(disconnecting->label_edit));
}
static void disconnect_confirmation_lifetime(void) {
    const int answers[] = {IDCANCEL, IDOK};
    for (int dirty = 0; dirty < 2; dirty++) {
        for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) {
            for (int retire = 0; retire < 2; retire++) {
                ScreenFixture f; screen_fixture_init(&f, ROUTES, 6);
                f.read_response = "{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"label\":\"Original\",\"status\":\"connected\"}],\"providers\":[\"gmail\"]}";
                Screen *base = mail_settings_screen_open(7);
                screen_fixture_pane(&f, base); pane_push(f.pane, base); screen_fixture_pump(&f, 1);
                disconnecting = (MailScreen *)base;
                CHECK(disconnecting->loaded); CHECK_INT(disconnecting->accounts.count, 1);
                HWND edit = disconnecting->label_edit; CHECK(IsWindow(edit));
                if (dirty) {
                    SetWindowTextW(edit, L"Draft");
                    base->vt->command(base, ID_MAIL_LABEL, EN_CHANGE, edit);
                }
                CHECK(disconnecting->dirty == (dirty != 0));
                modal_driver_action_during(base, ACT_DELETE, 7, answers[i], retire ? nested_close_then_retire : nested_close);
                if (retire) {
                    CHECK(pane_top(f.pane) == NULL); CHECK(!IsWindow(edit)); CHECK_INT(f.writes, 0);
                } else {
                    if (answers[i] == IDOK) {
                        screen_fixture_pump(&f, 2); CHECK_INT(f.writes, 1);
                        CHECK_STR(f.method, "DELETE");
                        CHECK_STR(f.url, "https://example.com/api/v1/settings/mail/accounts/7");
                    } else {
                        CHECK_INT(f.writes, 0); CHECK(disconnecting->dirty == (dirty != 0));
                        CHECK_OWNED_STR(screen_mail_edit_text(edit), dirty ? "Draft" : "Original");
                    }
                    CHECK(!disconnecting->modal); CHECK(IsWindow(edit));
                    pane_set_root(f.pane, NULL);
                }
                disconnecting = NULL; screen_fixture_clear(&f);
            }
        }
    }
}
void app_mail_tests(void) {
    screen_fixture_prepare();
    test_run("mail disconnect keeps its owner through nested close and teardown", disconnect_confirmation_lifetime);
    test_run("mail navigation and each write require deployed routes and admin permission", permissions);
    test_run("mail results are discarded on screen connection or permission changes", lifecycle);
    test_run("cancelled and superseded mail requests never reach the retired owner", cancellation);
    test_run("mail status polling is conservative and respects rate-limit delays", backoff);
}
