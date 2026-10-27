#include "screen_fixture.h"
#include "screen_webhook_internal.h"
#include "suites.h"

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
void app_webhook_tests(void) {
    screen_fixture_prepare();
    test_run("webhook form loads saves and reopens unlimited turns", unlimited_lifecycle);
    test_run("new webhook form keeps defaults and rejects invalid saves", defaults_and_invalid_save);
}
