#include "screen_fixture.h"
#include "screen_webhook_internal.h"
#include "suites.h"
#include "modal_driver.h"

static Route ROUTES[] = {
    {"GET", "/sessions/{sessionId}/webhook", "admin"},
    {"PUT", "/sessions/{sessionId}/webhook", "admin"},
};
static const char *UNLIMITED = "{\"armed\":true,\"perHour\":30,\"maxTurns\":0,\"instructions\":false,\"sshUnattended\":true}";

static Screen *open_form(ScreenFixture *f) {
    Session session = {0}; session.raw = json_parsez("{\"id\":\"hook-test\"}");
    Screen *s = webhook_screen_new(&session); session_free(&session);
    if (!f->pane) screen_fixture_pane(f, s);
    else s->pane = f->pane;
    s->vt->visible(s, true); screen_fixture_pump(f, 1);
    return s;
}
static void unlimited_lifecycle(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, 2);
    f.read_response = f.write_response = UNLIMITED;
    Screen *s = open_form(&f); WebhookForm *form = (WebhookForm *)s;
    CHECK_OWNED_STR(screen_webhook_edit_text(form->edits[F_MAX_TURNS]), "0");
    SetWindowTextW(form->edits[F_MAX_TURNS], L"1000");
    SetWindowTextW(form->edits[F_MAX_TURNS], L"0");
    s->vt->command(s, ID_FIELD + F_MAX_TURNS, EN_CHANGE, form->edits[F_MAX_TURNS]);
    CHECK(form->dirty);
    screen_webhook_form_save(form); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK_STR(f.method, "PUT");
    CHECK_STR(f.url, "https://example.com/api/v1/sessions/hook-test/webhook");
    CHECK_INT(json_int_or(json_get(f.body, "maxTurns"), -1), 0);
    CHECK_INT(json_int_or(json_get(f.body, "perHour"), -1), 30);
    CHECK(json_bool_is(json_get(f.body, "sshUnattended"), true));
    CHECK(!form->dirty); CHECK(!form->busy);
    CHECK_OWNED_STR(screen_webhook_edit_text(form->edits[F_MAX_TURNS]), "0");
    s->vt->destroy(s);
    s = open_form(&f); form = (WebhookForm *)s;
    CHECK_OWNED_STR(screen_webhook_edit_text(form->edits[F_MAX_TURNS]), "0");
    s->vt->destroy(s); screen_fixture_clear(&f);
}
static void defaults_and_invalid_save(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, 2); f.read_response = "{}";
    f.write_response = UNLIMITED;
    Screen *s = open_form(&f); WebhookForm *form = (WebhookForm *)s;
    CHECK_OWNED_STR(screen_webhook_edit_text(form->edits[F_PER_HOUR]), "30");
    CHECK_OWNED_STR(screen_webhook_edit_text(form->edits[F_MAX_TURNS]), "10");
    SetWindowTextW(form->edits[F_MAX_TURNS], L"-1");
    screen_webhook_form_save(form); CHECK_INT(f.writes, 0); CHECK(!form->busy);
    SetWindowTextW(form->edits[F_MAX_TURNS], L"bad");
    screen_webhook_form_save(form); CHECK_INT(f.writes, 0);
    SetWindowTextW(form->edits[F_MAX_TURNS], L"0");
    screen_webhook_form_save(form); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 1); CHECK_INT(json_int_or(json_get(f.body, "maxTurns"), -1), 0);
    s->vt->destroy(s); screen_fixture_clear(&f);
}
static WebhookForm *leaving_form;
static bool leave_result;
static void check_leave(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)action; (void)arg; (void)pt;
    leave_result = leaving_form->base.vt->can_leave(&leaving_form->base);
}
static void retire_form(void) {
    WebhookForm *s = leaving_form;
    pane_set_root(s->base.pane, NULL);
    CHECK(s->retired); CHECK(s->modal);
    CHECK(IsWindow(s->edits[F_PER_HOUR]));
}
static void leave_confirmation_lifetime(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, 2); f.read_response = UNLIMITED;
    static const ScreenVTable navigation_vt = {.action = check_leave};
    Screen navigation = {.vt = &navigation_vt};
    const int answers[] = {IDCANCEL, IDOK};
    for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) {
        Screen *base = open_form(&f); leaving_form = (WebhookForm *)base;
        pane_push(f.pane, base); screen_fixture_pump(&f, 1);
        leaving_form->dirty = true;
        modal_driver_action(&navigation, 0, 0, IDCANCEL);
        CHECK(!leave_result); CHECK(leaving_form->dirty);
        HWND edit = leaving_form->edits[F_PER_HOUR];
        modal_driver_action_during(&navigation, 0, 0, answers[i], retire_form);
        CHECK(leave_result); CHECK(pane_top(f.pane) == NULL); CHECK(!IsWindow(edit));
        leaving_form = NULL;
    }
    screen_fixture_clear(&f);
}
void app_webhook_tests(void) {
    screen_fixture_prepare();
    test_run("webhook leave confirmation survives stack teardown", leave_confirmation_lifetime);
    test_run("webhook form loads saves and reopens unlimited turns", unlimited_lifecycle);
    test_run("new webhook form keeps defaults and rejects invalid saves", defaults_and_invalid_save);
}
