#include "screen_fixture.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/mail/accounts", "admin"},
    {"POST", "/settings/mail/accounts/connect", "admin"},
};
static const char *ACCOUNTS = "{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"status\":\"connected\"}],\"providers\":[\"gmail\"]}";

static Screen *setup(ScreenFixture *f, int account) {
    screen_fixture_init(f, ROUTES, 2); f->read_response = ACCOUNTS;
    // A rejected exchange lets us inspect consent without launching a browser.
    f->write_status = 409;
    Screen *s = account ? mail_settings_screen_open(account) : mail_settings_screen_new(); screen_fixture_pane(f, s); s->vt->visible(s, true); screen_fixture_pump(f, 1); return s;
}
static void explicit_management(void) {
    ScreenFixture f; Screen *s = setup(&f, 7); Doc doc; doc_init(&doc);
    f.block_write = true;
    screen_fixture_draw(s, &doc); int i = screen_fixture_find(&doc, "Enable mail actions"); CHECK(i >= 0);
    if (i >= 0) {
        Item button = doc.items[i]; CHECK(button.action != 0);
        s->vt->action(s, button.action, button.arg, (POINT){0});
        CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
        CHECK_INT(f.writes, 1); CHECK_STR(f.method, "POST");
        CHECK_STR(f.url, "https://example.com/api/v1/settings/mail/accounts/connect");
        CHECK_STR(json_str(json_get(f.body, "provider")), "gmail");
        CHECK_INT(json_int_or(json_get(f.body, "accountId"), 0), 7);
        CHECK_STR(json_str(json_get(f.body, "access")), "manage");
        s->vt->action(s, button.action, button.arg, (POINT){0}); CHECK_INT(f.writes, 1);
        s->vt->visible(s, false); SetEvent(f.resume); screen_fixture_pump(&f, 1);
        screen_fixture_draw(s, &doc); CHECK(screen_fixture_find(&doc, "one@example.com") < 0);
    }
    s->vt->destroy(s); doc_free(&doc); screen_fixture_clear(&f);
}
static void ordinary_sign_in(void) {
    const char *actions[] = {"Sign in again", "Connect Gmail"};
    for (size_t i = 0; i < sizeof actions / sizeof *actions; i++) {
        ScreenFixture f; Screen *s = setup(&f, i ? 0 : 7); Doc doc; doc_init(&doc);
        screen_fixture_click(s, &doc, actions[i]); screen_fixture_pump(&f, 1);
        CHECK_INT(f.writes, 1); CHECK_STR(json_str(json_get(f.body, "provider")), "gmail");
        if (!i) CHECK_INT(json_int_or(json_get(f.body, "accountId"), 0), 7);
        else CHECK(json_is_null(json_get(f.body, "accountId")));
        CHECK(json_is_null(json_get(f.body, "access")));
        s->vt->destroy(s); doc_free(&doc); screen_fixture_clear(&f);
    }
}
static void revoked_permissions(void) {
    for (int missing_route = 0; missing_route < 2; missing_route++) {
        ScreenFixture f; Screen *s = setup(&f, 7); Doc doc; doc_init(&doc);
        screen_fixture_draw(s, &doc); int i = screen_fixture_find(&doc, "Enable mail actions"); CHECK(i >= 0);
        if (i >= 0) {
            Item button = doc.items[i];
            if (missing_route) g_store.route_count = 1; else g_store.device.permission = "read";
            s->vt->action(s, button.action, button.arg, (POINT){0}); CHECK_INT(f.writes, 0);
        }
        s->vt->destroy(s); doc_free(&doc); screen_fixture_clear(&f);
    }
}
void app_mail_consent_tests(void) {
    screen_fixture_prepare();
    test_run("mail management consent sends the selected account and cancels hidden results", explicit_management);
    test_run("ordinary mail sign-in does not request additional permissions", ordinary_sign_in);
    test_run("mail management consent requires admin permission and the connect route", revoked_permissions);
}
