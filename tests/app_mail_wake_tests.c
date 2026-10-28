#include "screen_fixture.h"
#include "screen_mail_wake_internal.h"
#include "suites.h"
#include "modal_driver.h"
#include <time.h>

static Route ROUTES[] = {
    {"GET", "/sessions/{id}/mail-wake", "admin"}, {"PUT", "/sessions/{id}/mail-wake", "admin"},
    {"GET", "/settings/mail/accounts", "admin"}, {"GET", "/settings/projects", "admin"},
};
static const char *EMPTY = "{\"rules\":[],\"perHour\":30,\"maxTurns\":10}";
static const char *SAVED = "{\"rules\":[{\"id\":\"server-id\",\"account\":7,\"enabled\":true,\"to\":\"\",\"from\":\"\",\"subject\":\"Alert\"}],\"perHour\":30,\"maxTurns\":10}";
static const char *ACCOUNTS = "{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"me@example.com\",\"enabled\":true,\"status\":\"connected\",\"access\":\"read\"}],\"providers\":[\"gmail\"]}";
typedef struct {
    ScreenFixture f;
    const char *responses[MW_ROUTES], *retry[MW_ROUTES]; int status[MW_ROUTES];
    volatile LONG calls[MW_ROUTES];
    HANDLE entered[MW_ROUTES], release[MW_ROUTES]; bool block[MW_ROUTES], fail_write;
} WakeFixture;
static bool wake_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *bytes, size_t size, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; WakeFixture *f = ctx;
    int route = str_eq(method, "PUT") ? MW_WRITE : str_has_suffix(url, "/mail-wake") ? MW_READ
        : str_has_suffix(url, "/settings/projects") ? MW_PROJECTS : MW_ACCOUNTS;
    // Snapshot each response before opening its gate. Later fixtures cannot change a result already in flight.
    *status = f->status[route]; *type = xstrdup("application/json"); *retry = xstrdup(f->retry[route]);
    *response = xstrdup(f->responses[route]); *len = strlen(*response); *error = NULL;
    if (route == MW_WRITE) {
        free(f->f.method); free(f->f.url); json_free(f->f.body);
        f->f.method = xstrdup(method); f->f.url = xstrdup(url);
        char *text = xstrndup(bytes, size); f->f.body = json_parsez(text); free(text);
    }
    bool fail = f->fail_write && route == MW_WRITE, block = f->block[route];
    InterlockedIncrement(&f->calls[route]);
    if (block) { SetEvent(f->entered[route]); WaitForSingleObject(f->release[route], 10000); }
    if (fail) { *error = xstrdup("Fixture timeout"); return false; }
    return true;
}
static void init(WakeFixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->f, ROUTES, 4);
    f->responses[MW_READ] = f->responses[MW_WRITE] = EMPTY; f->responses[MW_ACCOUNTS] = ACCOUNTS;
    f->responses[MW_PROJECTS] = "{\"projects\":[{\"repo\":\"o/r\",\"mailToolsEnabled\":true}]}";
    for (int i = 0; i < MW_ROUTES; i++) {
        f->status[i] = 200; f->entered[i] = CreateEventW(NULL, TRUE, FALSE, NULL); f->release[i] = CreateEventW(NULL, TRUE, FALSE, NULL);
    }
    api_client_set_transport(f->f.client, wake_transport, f);
}
static void clear(WakeFixture *f) {
    for (int i = 0; i < MW_ROUTES; i++) { CloseHandle(f->entered[i]); CloseHandle(f->release[i]); }
    json_free(g_store.mail_wake_state); g_store.mail_wake_state = NULL; screen_fixture_clear(&f->f);
}
static MailWakeScreen *open_form(WakeFixture *f, const char *id, int completions) {
    Session ss = {0}; ss.raw = json_object(); json_set_str(ss.raw, "id", id); json_set_str(ss.raw, "repo", "o/r");
    Screen *base = mail_wake_screen_new(&ss); session_free(&ss);
    if (!f->f.pane) screen_fixture_pane(&f->f, base); else base->pane = f->f.pane;
    base->vt->visible(base, true); if (completions) screen_fixture_pump(&f->f, completions); return (MailWakeScreen *)base;
}
static void act(MailWakeScreen *s, int action, intptr_t arg) { s->base.vt->action(&s->base, action, arg, (POINT){0}); }
static void edit(MailWakeScreen *s, int field, const wchar_t *text) {
    SetWindowTextW(s->edits[field], text); s->base.vt->command(&s->base, MW_FIELD_ID + field, EN_CHANGE, s->edits[field]);
}
static void close_form(MailWakeScreen *s) { s->base.vt->destroy(&s->base); }
static void lifecycle(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s/+", 3);
    CHECK(s->fresh[MW_READ] && s->fresh[MW_ACCOUNTS] && s->fresh[MW_PROJECTS]);
    CHECK(!mail_wake_dirty(&s->draft)); act(s, MW_ADD, 0); CHECK_INT(json_count(s->draft.rules), 1);
    CHECK_INT(json_int_or(json_get(json_at(s->draft.rules, 0), "account"), 0), 7);
    edit(s, 4, L"Alert"); CHECK(mail_wake_dirty(&s->draft));
    act(s, MW_TOGGLE, 0); CHECK(json_bool_is(json_get(json_at(s->draft.rules, 0), "enabled"), false)); act(s, MW_TOGGLE, 0);
    act(s, MW_PICK, 0); CHECK_INT(s->picker, 0); act(s, MW_SELECT, 999); CHECK_INT(s->picker, 0); act(s, MW_SELECT, 7); CHECK_INT(s->picker, -1);
    act(s, MW_ADD, 0); CHECK_INT(json_count(s->draft.rules), 2); act(s, MW_REMOVE, 1); CHECK_INT(json_count(s->draft.rules), 1);
    edit(s, 1, L"0"); act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 0); CHECK(s->errors[MW_WRITE] != NULL);
    edit(s, 1, L"10"); f.responses[MW_WRITE] = SAVED;
    CHECK(mail_wake_savable(s)); CHECK(s->base.vt->key(&s->base, 'S', true, false)); act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1);
    CHECK_INT(f.calls[MW_WRITE], 1); CHECK_STR(f.f.method, "PUT"); CHECK_STR(f.f.url, "https://example.com/api/v1/sessions/s%2F%2B/mail-wake");
    CHECK_INT(json_count(f.f.body), 3); CHECK_STR(json_str(json_get(json_at(json_get(f.f.body, "rules"), 0), "subject")), "Alert");
    CHECK(!mail_wake_dirty(&s->draft)); CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "id")), "server-id");
    edit(s, 4, L"draft kept"); s->base.vt->refresh(&s->base); screen_fixture_pump(&f.f, 3);
    CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "draft kept");
    s->base.vt->visible(&s->base, false); s->base.vt->visible(&s->base, true); screen_fixture_pump(&f.f, 3);
    CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "draft kept");
    mail_wake_discard(&s->draft); mail_wake_fill_controls(s); CHECK(!mail_wake_dirty(&s->draft));
    close_form(s); f.responses[MW_READ] = SAVED; s = open_form(&f, "s/+", 3);
    CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "Alert"); close_form(s); clear(&f);
}
static void gating(void) {
    WakeFixture f; init(&f); g_store.device.permission = "manage"; MailWakeScreen *s = open_form(&f, "s", 0);
    CHECK(!mail_wake_current(s)); CHECK(!mail_wake_editable(s)); act(s, MW_ADD, 0); act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_READ], 0); close_form(s);
    g_store.device.permission = "admin"; g_store.route_count = 1; s = open_form(&f, "s", 1);
    CHECK(!mail_wake_editable(s)); CHECK(s->errors[MW_ACCOUNTS] != NULL); CHECK(s->errors[MW_PROJECTS] != NULL); close_form(s);
    g_store.route_count = 4; f.responses[MW_PROJECTS] = "{\"projects\":[{\"repo\":\"other/repo\",\"mailToolsEnabled\":true}]}";
    s = open_form(&f, "s", 3); CHECK(!s->fresh[MW_PROJECTS]); act(s, MW_ADD, 0); CHECK_INT(json_count(s->draft.rules), 0);
    f.responses[MW_READ] = "{\"rules\":[{\"id\":\"old\",\"account\":999,\"enabled\":false,\"subject\":\"keep\"}],\"perHour\":30,\"maxTurns\":10}";
    f.responses[MW_PROJECTS] = "[{\"repo\":\"o/r\",\"mailToolsEnabled\":false}]";
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.f, 3); CHECK_INT(s->tools, 0);
    act(s, MW_TOGGLE, 0); CHECK(json_bool_is(json_get(json_at(s->draft.rules, 0), "enabled"), false));
    edit(s, 4, L"editable disabled mailbox"); CHECK(mail_wake_dirty(&s->draft));
    f.responses[MW_WRITE] = "{\"rules\":[{\"id\":\"old\",\"account\":999,\"enabled\":false,\"to\":\"\",\"from\":\"\",\"subject\":\"editable disabled mailbox\"}],\"perHour\":30,\"maxTurns\":10}";
    act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1); CHECK(!mail_wake_dirty(&s->draft)); close_form(s); clear(&f);
}
static void independent_errors(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0); edit(s, 4, L"local");
    f.status[MW_ACCOUNTS] = 429; f.retry[MW_ACCOUNTS] = "120";
    f.status[MW_PROJECTS] = 503; f.retry[MW_PROJECTS] = "60"; f.status[MW_READ] = 500;
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.f, 3);
    CHECK(s->errors[MW_READ] && s->errors[MW_ACCOUNTS] && s->errors[MW_PROJECTS]);
    CHECK(!mail_wake_savable(s)); double account_deadline = mail_wake_deadline(s, MW_ACCOUNTS), project_deadline = mail_wake_deadline(s, MW_PROJECTS);
    CHECK(account_deadline > (double)time(NULL)); CHECK(project_deadline > (double)time(NULL));
    f.status[MW_READ] = 200; act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 1);
    CHECK(s->errors[MW_READ] == NULL); CHECK(s->errors[MW_ACCOUNTS] && s->errors[MW_PROJECTS]);
    CHECK(mail_wake_deadline(s, MW_ACCOUNTS) == account_deadline); CHECK(mail_wake_deadline(s, MW_PROJECTS) == project_deadline);
    CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "local");
    close_form(s); s = open_form(&f, "other", 1); CHECK_INT(f.calls[MW_ACCOUNTS], 2); CHECK_INT(f.calls[MW_PROJECTS], 2); close_form(s); clear(&f);
}
static void uncertain_and_refused(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0); edit(s, 4, L"draft");
    f.status[MW_WRITE] = 400; act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1);
    CHECK(mail_wake_dirty(&s->draft)); CHECK(!json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true));
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); CHECK(mail_wake_savable(s));
    f.status[MW_WRITE] = 200; f.fail_write = true; act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1);
    CHECK(json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true)); CHECK(mail_wake_dirty(&s->draft));
    act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 2);
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); CHECK(!mail_wake_savable(s)); CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "draft");
    close_form(s); s = open_form(&f, "s", 3); act(s, MW_ADD, 0); act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 2); close_form(s); clear(&f);
}
static void cancelled_completion(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0); edit(s, 4, L"draft");
    f.block[MW_WRITE] = true; f.responses[MW_WRITE] = SAVED; act(s, MW_SAVE, 0);
    CHECK_INT(WaitForSingleObject(f.entered[MW_WRITE], 10000), WAIT_OBJECT_0); close_form(s);
    s = open_form(&f, "s", 2); CHECK(json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "pending"), true));
    act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 1);
    SetEvent(f.release[MW_WRITE]); screen_fixture_pump(&f.f, 1);
    CHECK(json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true)); CHECK(!json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "pending"), true));
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); CHECK(s->fresh[MW_READ]); CHECK(!mail_wake_savable(s)); close_form(s); clear(&f);
}
static void stale_reads(void) {
    WakeFixture f; init(&f); f.block[MW_READ] = f.block[MW_ACCOUNTS] = f.block[MW_PROJECTS] = true;
    MailWakeScreen *s = open_form(&f, "old", 0);
    for (int i = 0; i < MW_WRITE; i++) CHECK_INT(WaitForSingleObject(f.entered[i], 10000), WAIT_OBJECT_0);
    s->base.vt->visible(&s->base, false);
    for (int i = 0; i < MW_WRITE; i++) { f.block[i] = false; SetEvent(f.release[i]); }
    screen_fixture_pump(&f.f, 3); CHECK(s->draft.rules == NULL); CHECK_INT(s->accounts.count, 0); CHECK_INT(s->tools, -1);
    close_form(s); s = open_form(&f, "new", 3); CHECK(s->fresh[MW_READ]);
    f.responses[MW_READ] = "{\"sessionId\":\"old\",\"rules\":[]}";
    s->base.vt->refresh(&s->base); screen_fixture_pump(&f.f, 3); CHECK(!s->fresh[MW_READ]); CHECK(s->errors[MW_READ] != NULL);
    f.responses[MW_READ] = EMPTY; f.block[MW_ACCOUNTS] = true; ResetEvent(f.entered[MW_ACCOUNTS]); ResetEvent(f.release[MW_ACCOUNTS]);
    s->base.vt->refresh(&s->base); CHECK_INT(WaitForSingleObject(f.entered[MW_ACCOUNTS], 10000), WAIT_OBJECT_0); screen_fixture_pump(&f.f, 2);
    g_store.device.permission = "manage"; SetEvent(f.release[MW_ACCOUNTS]); screen_fixture_pump(&f.f, 1);
    CHECK(!mail_wake_current(s)); CHECK(!mail_wake_savable(s)); close_form(s); clear(&f);
}
static void write_cooldown_and_cancelled_read(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0);
    f.status[MW_WRITE] = 429; f.retry[MW_WRITE] = "120"; act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1);
    double deadline = mail_wake_deadline(s, MW_WRITE); CHECK(deadline > (double)time(NULL));
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 1);
    close_form(s); s = open_form(&f, "s", 3); act(s, MW_ADD, 0); act(s, MW_SAVE, 0); CHECK_INT(f.calls[MW_WRITE], 1);
    CHECK(mail_wake_deadline(s, MW_WRITE) == deadline);
    f.block[MW_ACCOUNTS] = true; f.status[MW_ACCOUNTS] = 429; f.retry[MW_ACCOUNTS] = "180";
    act(s, MW_RETRY, 0); CHECK_INT(WaitForSingleObject(f.entered[MW_ACCOUNTS], 10000), WAIT_OBJECT_0); screen_fixture_pump(&f.f, 2);
    close_form(s); SetEvent(f.release[MW_ACCOUNTS]); screen_fixture_pump(&f.f, 1);
    s = open_form(&f, "s", 2); CHECK(mail_wake_deadline(s, MW_ACCOUNTS) > deadline); CHECK_INT(f.calls[MW_ACCOUNTS], 4);
    close_form(s); clear(&f);
}
static void malformed_prerequisites_and_saved_response(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0); edit(s, 4, L"keep");
    f.responses[MW_ACCOUNTS] = "{\"accounts\":[{\"id\":999}],\"providers\":[]}";
    f.responses[MW_PROJECTS] = "{\"projects\":[{\"repo\":\"other\",\"mailToolsEnabled\":true}]}";
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); CHECK(!s->fresh[MW_ACCOUNTS]); CHECK(!s->fresh[MW_PROJECTS]);
    CHECK(s->errors[MW_ACCOUNTS] && s->errors[MW_PROJECTS]); CHECK(!mail_wake_savable(s));
    CHECK_STR(json_str(json_get(json_at(s->draft.rules, 0), "subject")), "keep");
    f.responses[MW_ACCOUNTS] = ACCOUNTS; f.responses[MW_PROJECTS] = "[{\"repo\":\"o/r\",\"mailToolsEnabled\":true}]";
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3); CHECK(mail_wake_savable(s));
    // A success with another record's contents is an uncertain outcome, even without an identity field.
    f.responses[MW_WRITE] = EMPTY; act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1);
    CHECK(mail_wake_dirty(&s->draft)); CHECK(json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true));
    close_form(s); clear(&f);
}
static void old_server_completion(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3);
    for (int i = 0; i < MW_WRITE; i++) { f.block[i] = true; f.status[i] = 401; }
    act(s, MW_RETRY, 0);
    for (int i = 0; i < MW_WRITE; i++) CHECK_INT(WaitForSingleObject(f.entered[i], 10000), WAIT_OBJECT_0);
    ServerAddress address; ApiError error; api_error_init(&error); CHECK(server_address_parse("https://other.example.com", &address));
    ApiClient *other = api_client_new(&address, "brm_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &error);
    server_address_free(&address); api_error_clear(&error); CHECK(other != NULL); g_store.client = other;
    for (int i = 0; i < MW_WRITE; i++) SetEvent(f.release[i]);
    screen_fixture_pump(&f.f, 3); CHECK(g_store.client == other); CHECK(g_store.has_device); CHECK(!mail_wake_current(s));
    CHECK(!mail_wake_savable(s)); close_form(s); api_client_release(other); g_store.client = f.f.client; clear(&f);
}
static void empty_layout(Screen *base, Doc *doc) { (void)base; (void)doc; }
static void empty_header(Screen *base, HeaderInfo *info) { (void)base; snprintf(info->title, sizeof info->title, "Test root"); }
static Screen *empty_root(void) {
    static const ScreenVTable vt = {.destroy = screen_release, .layout = empty_layout, .header = empty_header};
    Screen *base = xcalloc(1, sizeof *base); base->vt = &vt; return base;
}
static Pane *navigation_pane;
static MailWakeScreen *retiring_screen;
static void navigate(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)action; (void)arg; (void)pt; pane_pop(navigation_pane);
}
static void retire_in_modal(void) { close_form(retiring_screen); }
static void discard_navigation(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3);
    pane_push(f.f.pane, empty_root()); pane_push(f.f.pane, &s->base); screen_fixture_pump(&f.f, 3);
    act(s, MW_ADD, 0); edit(s, 4, L"draft");
    navigation_pane = f.f.pane;
    static const ScreenVTable navigation_vt = {.action = navigate}; Screen navigation = {.vt = &navigation_vt};
    modal_driver_action(&navigation, 0, 0, IDCANCEL); CHECK_INT(pane_depth(f.f.pane), 2); CHECK(mail_wake_dirty(&s->draft));
    modal_driver_action(&s->base, MW_DISCARD, 0, IDCANCEL); CHECK(mail_wake_dirty(&s->draft));
    modal_driver_action(&s->base, MW_DISCARD, 0, IDOK); CHECK(!mail_wake_dirty(&s->draft)); CHECK_INT(json_count(s->draft.rules), 0);
    act(s, MW_ADD, 0); modal_driver_action(&navigation, 0, 0, IDOK); CHECK_INT(pane_depth(f.f.pane), 1);
    pane_set_root(f.f.pane, NULL); navigation_pane = NULL; clear(&f);
}
static void reconcile_and_modal_retirement(void) {
    WakeFixture f; init(&f); MailWakeScreen *s = open_form(&f, "s", 3); act(s, MW_ADD, 0); edit(s, 4, L"draft");
    f.responses[MW_WRITE] = "{\"sessionId\":\"other\",\"rules\":[]}";
    act(s, MW_SAVE, 0); screen_fixture_pump(&f.f, 1); CHECK(mail_wake_dirty(&s->draft));
    CHECK(json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true));
    act(s, MW_RETRY, 0); screen_fixture_pump(&f.f, 3);
    modal_driver_action(&s->base, MW_RECONCILE, 0, IDCANCEL); CHECK(mail_wake_dirty(&s->draft));
    modal_driver_action(&s->base, MW_RECONCILE, 0, IDOK); CHECK(!mail_wake_dirty(&s->draft));
    CHECK(!json_bool_is(json_get(mail_wake_entry(s, MW_WRITE), "uncertain"), true)); CHECK_INT(f.calls[MW_WRITE], 1);
    act(s, MW_ADD, 0); f.block[MW_ACCOUNTS] = true; act(s, MW_RETRY, 0);
    CHECK_INT(WaitForSingleObject(f.entered[MW_ACCOUNTS], 10000), WAIT_OBJECT_0); screen_fixture_pump(&f.f, 2);
    retiring_screen = s; modal_driver_action_during(&s->base, MW_DISCARD, 0, IDOK, retire_in_modal); retiring_screen = NULL;
    SetEvent(f.release[MW_ACCOUNTS]); screen_fixture_pump(&f.f, 1); clear(&f);
}
static bool detached_leave_result;
static void check_detached_leave(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)action; (void)arg; (void)pt; detached_leave_result = detached_can_leave_all();
}
static void detached_navigation(void) {
    WakeFixture f; init(&f);
    Pane *pane = detached_open(empty_root(), NULL, false); CHECK(pane != NULL);
    Session ss = {0}; ss.raw = json_parsez("{\"id\":\"detached\",\"repo\":\"o/r\"}");
    MailWakeScreen *s = (MailWakeScreen *)mail_wake_screen_new(&ss); session_free(&ss); pane_push(pane, &s->base); screen_fixture_pump(&f.f, 3);
    act(s, MW_ADD, 0); edit(s, 4, L"detached draft");
    static const ScreenVTable vt = {.action = check_detached_leave}; Screen navigation = {.vt = &vt};
    modal_driver_action(&navigation, 0, 0, IDCANCEL); CHECK(!detached_leave_result); CHECK(mail_wake_dirty(&s->draft)); CHECK_INT(pane_depth(pane), 2);
    modal_driver_action(&navigation, 0, 0, IDOK); CHECK(detached_leave_result); CHECK(!mail_wake_dirty(&s->draft));
    detached_close_all(); clear(&f);
}
void app_mail_wake_tests(void) {
    screen_fixture_prepare();
    test_run("mail wake late old-server 401 cannot invalidate the replacement connection", old_server_completion);
    test_run("main-window pre-close guard includes unsaved detached mail wake drafts", detached_navigation);
    test_run("mail wake PUT cooldown and cancelled read Retry-After survive reopening", write_cooldown_and_cancelled_read);
    test_run("mail wake malformed prerequisites and wrong saved contents preserve draft", malformed_prerequisites_and_saved_response);
    test_run("mail wake Back cancellation and explicit discard guard stacked navigation", discard_navigation);
    test_run("mail wake explicit reconciliation and modal retirement drop later completions", reconcile_and_modal_retirement);
    test_run("mail wake linked form add/edit/remove/enable/save/refill/reopen", lifecycle);
    test_run("mail wake catalog/token/project gating and unavailable disabled accounts", gating);
    test_run("mail wake independent errors and route cooldowns survive refresh/reopen", independent_errors);
    test_run("mail wake refusal and uncertain save preserve drafts without duplicate writes", uncertain_and_refused);
    test_run("mail wake cancelled write finalizer survives teardown and blocks reopened writes", cancelled_completion);
    test_run("mail wake hidden, wrong-session and changed-device reads are dropped", stale_reads);
}
