#include "screens.h"
#include "mail_settings.h"
#include "suites.h"
#include "test.h"
#include <string.h>

Screen *consent_test_screen_new(void);
Screen *consent_test_screen_open(int account_id);
bool consent_test_offered(void);
bool consent_test_current(bool shown, const Request *r);
int consent_test_retry(int failures, double retry_after);
static Json *sent;
static int writes;
static Request pending_request;
static void consent_repaint(Pane *pane) { (void)pane; }
static Request *consent_call(const char *op, Json *args, int timeout, void *owner, RequestDone done, int tag, Request **slot) {
    (void)timeout; (void)owner; (void)done; (void)tag;
    CHECK_STR(op, "connect_mail_account");
    writes++; sent = args;
    memset(&pending_request, 0, sizeof pending_request);
    *slot = &pending_request;
    return &pending_request;
}
#define mail_settings_screen_new consent_test_screen_new
#define mail_settings_screen_open consent_test_screen_open
#define mail_settings_offered consent_test_offered
#define mail_settings_result_current consent_test_current
#define mail_settings_retry_ms consent_test_retry
#define store_call consent_call
#define pane_relayout consent_repaint
#define pane_header_changed consent_repaint
#include "../app/screen_mail.c"
#undef mail_settings_screen_new
#undef mail_settings_screen_open
#undef mail_settings_offered
#undef mail_settings_result_current
#undef mail_settings_retry_ms
#undef store_call
#undef pane_relayout
#undef pane_header_changed

static Route CONSENT_ROUTES[] = {
    { "GET", "/settings/mail/accounts", "admin" },
    { "POST", "/settings/mail/accounts/connect", "admin" },
};
static MailScreen *consent_setup(void) {
    memset(&g_store, 0, sizeof g_store);
    g_store.has_device = true; g_store.device.permission = "admin";
    g_store.routes = CONSENT_ROUTES; g_store.route_count = 2;
    MailScreen *s = (MailScreen *)consent_test_screen_new();
    s->shown = s->loaded = true;
    Json *accounts = json_parsez("{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"status\":\"connected\"}],\"providers\":[\"gmail\"]}");
    CHECK(mail_accounts_parse(accounts, &s->accounts)); json_free(accounts);
    sent = NULL; writes = 0;
    return s;
}
static void consent_cleanup(MailScreen *s) {
    s->write = NULL; release(s); json_free(sent); sent = NULL;
    memset(&g_store, 0, sizeof g_store);
}
static void explicit_management(void) {
    MailScreen *s = consent_setup();
    action(&s->base, ACT_MANAGE, 7, (POINT){0});
    CHECK_INT(writes, 1);
    CHECK_STR(json_str(json_get(sent, "provider")), "gmail");
    CHECK_INT(json_int_or(json_get(sent, "accountId"), 0), 7);
    CHECK_STR(json_str(json_get(sent, "access")), "manage");
    action(&s->base, ACT_MANAGE, 7, (POINT){0}); CHECK_INT(writes, 1);
    consent_cleanup(s);
}
static void ordinary_sign_in(void) {
    MailScreen *s = consent_setup();
    action(&s->base, ACT_REAUTH, 7, (POINT){0});
    CHECK_INT(writes, 1);
    CHECK_INT(json_int_or(json_get(sent, "accountId"), 0), 7);
    CHECK(json_get(sent, "access") == NULL);
    consent_cleanup(s);
    s = consent_setup();
    action(&s->base, ACT_CONNECT, 0, (POINT){0});
    CHECK_INT(writes, 1);
    CHECK(json_get(sent, "accountId") == NULL);
    CHECK(json_get(sent, "access") == NULL);
    consent_cleanup(s);
}
static void revoked_permissions(void) {
    MailScreen *s = consent_setup(); g_store.device.permission = "read";
    action(&s->base, ACT_MANAGE, 7, (POINT){0}); CHECK_INT(writes, 0);
    consent_cleanup(s);
    s = consent_setup(); g_store.route_count = 1;
    action(&s->base, ACT_MANAGE, 7, (POINT){0}); CHECK_INT(writes, 0);
    consent_cleanup(s);
}
void app_mail_consent_tests(void) {
    test_run("enable mail actions requests management consent for the selected account", explicit_management);
    test_run("ordinary mail sign-in does not request additional permissions", ordinary_sign_in);
    test_run("mail management consent requires admin permission and the connect route", revoked_permissions);
}
