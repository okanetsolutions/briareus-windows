#include "screen_fixture.h"
#include "modal_driver.h"
#include "mail_inbox.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/mail/accounts", "admin"}, {"GET", "/mail/messages", "admin"},
    {"GET", "/mail/accounts/{account}/messages/{id}", "admin"},
    {"DELETE", "/mail/accounts/{account}/messages/{id}", "admin"},
};
static const char *MAIL = "{\"accounts\":[{\"id\":7,\"provider\":\"gmail\",\"email\":\"one@example.com\",\"status\":\"connected\"},{\"id\":8,\"provider\":\"outlook\",\"email\":\"two@example.com\",\"status\":\"connected\"}],\"providers\":[\"gmail\",\"outlook\"],\"messages\":[{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"Selected\",\"receivedAt\":200,\"isRead\":true},{\"accountId\":8,\"id\":\"same/+=\",\"subject\":\"Other mailbox\",\"receivedAt\":100,\"isRead\":true}],\"nextCursor\":\"older\",\"message\":{\"accountId\":7,\"id\":\"same/+=\",\"subject\":\"Selected\",\"body\":{\"text\":\"private selected body\"}}}";

static Screen *setup(ScreenFixture *f, Doc *doc, Item *button) {
    screen_fixture_init(f, ROUTES, 4); f->read_response = MAIL; f->write_response = "{\"ok\":true}";
    Screen *s = mail_screen_new(); doc_init(doc); s->vt->visible(s, true); screen_fixture_pump(f, 2);
    screen_fixture_select(s, doc, "Selected"); screen_fixture_pump(f, 1);
    screen_fixture_draw(s, doc); int i = screen_fixture_find(doc, "Delete message"); CHECK(i >= 0);
    if (i >= 0) { *button = doc->items[i]; CHECK(button->action != 0); }
    return s;
}
static void cleanup(Screen *s, Doc *doc, ScreenFixture *f) {
    s->vt->destroy(s); doc_free(doc); screen_fixture_clear(f);
}
static bool revoked_in_modal;
static void revoke_during_confirmation(void) {
    g_store.device.permission = "read";
    revoked_in_modal = true;
}
static void delete_requires_confirmation(void) {
    ScreenFixture f; Doc doc; Item button = {0}; Screen *s = setup(&f, &doc, &button);
    modal_driver_action(s, button.action, button.arg, IDCANCEL); screen_fixture_pump(&f, 1);
    CHECK_INT(f.writes, 0); screen_fixture_draw(s, &doc);
    CHECK(screen_fixture_find(&doc, "private selected body") >= 0);
    g_store.device.permission = "read";
    s->vt->action(s, button.action, button.arg, (POINT){0}); CHECK_INT(f.writes, 0);
    g_store.device.permission = "admin"; g_store.route_count = 3;
    s->vt->action(s, button.action, button.arg, (POINT){0}); CHECK_INT(f.writes, 0);
    g_store.route_count = 4; revoked_in_modal = false;
    modal_driver_action_during(s, button.action, button.arg, IDOK, revoke_during_confirmation);
    CHECK(revoked_in_modal); CHECK_INT(f.writes, 0);
    cleanup(s, &doc, &f);
}
static void delete_selected_only(void) {
    ScreenFixture f; Doc doc; Item button = {0}; Screen *s = setup(&f, &doc, &button);
    f.block_write = true;
    modal_driver_action(s, button.action, button.arg, IDOK);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    CHECK_INT(f.writes, 1); CHECK_STR(f.method, "DELETE");
    CHECK_STR(f.url, "https://example.com/api/v1/mail/accounts/7/messages/same%2F%2B%3D");
    s->vt->action(s, button.action, button.arg, (POINT){0}); CHECK_INT(f.writes, 1);
    SetEvent(f.resume); screen_fixture_pump(&f, 2); screen_fixture_draw(s, &doc);
    CHECK(screen_fixture_find(&doc, "Selected") < 0);
    CHECK(screen_fixture_find(&doc, "Other mailbox") >= 0);
    CHECK(screen_fixture_find(&doc, "private selected body") < 0);
    int more = screen_fixture_find(&doc, "Load older messages"); CHECK(more >= 0);
    if (more >= 0) CHECK(doc.items[more].action != 0);
    cleanup(s, &doc, &f);
}
static void delete_failure_preserves_message(void) {
    const int statuses[] = {409, 429, 502};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        ScreenFixture f; Doc doc; Item button = {0}; Screen *s = setup(&f, &doc, &button);
        f.write_status = statuses[i]; f.write_response = "{\"error\":\"private provider secret\"}";
        modal_driver_action(s, button.action, button.arg, IDOK); screen_fixture_pump(&f, 1);
        CHECK_INT(f.writes, 1); screen_fixture_draw(s, &doc);
        CHECK(screen_fixture_find(&doc, "Selected") >= 0);
        CHECK(screen_fixture_find(&doc, "Other mailbox") >= 0);
        CHECK(screen_fixture_find(&doc, "private selected body") >= 0);
        for (size_t k = 0; k < doc.count; k++) if (doc.items[k].text)
            CHECK(strstr(doc.items[k].text, "private provider secret") == NULL);
        cleanup(s, &doc, &f);
    }
}
static void deletion_respects_lifecycle(void) {
    ScreenFixture f; Doc doc; Item button = {0}; Screen *s = setup(&f, &doc, &button);
    f.block_write = true; modal_driver_action(s, button.action, button.arg, IDOK);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    s->vt->visible(s, false); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    screen_fixture_draw(s, &doc);
    CHECK(screen_fixture_find(&doc, "Selected") < 0);
    CHECK(screen_fixture_find(&doc, "private selected body") < 0);
    cleanup(s, &doc, &f);
}
void app_mail_delete_tests(void) {
    screen_fixture_prepare();
    test_run("mail deletion requires actual confirmation, current admin access and its route", delete_requires_confirmation);
    test_run("mail deletion removes only the selected mailbox message and blocks repeat clicks", delete_selected_only);
    test_run("mail deletion preserves messages after refused or uncertain writes", delete_failure_preserves_message);
    test_run("mail deletion discards hidden completion and clears private content", deletion_respects_lifecycle);
}
