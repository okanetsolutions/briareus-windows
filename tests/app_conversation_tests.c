#include "screen_fixture.h"
#include "screen_conversation_internal.h"
#include "suites.h"

static Route ROUTES[] = {{"GET", "/sessions/{sessionId}", "read"}};

static Screen *open_conversation(ScreenFixture *f) {
    Session session = {0}; session.raw = json_parsez("{\"id\":\"background-test\",\"status\":\"running\"}");
    Screen *screen = conversation_screen_new(&session); session_free(&session);
    if (!f->pane) screen_fixture_pane(f, screen); else screen->pane = f->pane;
    return screen;
}
static void refresh(ScreenFixture *f, Screen *screen, const char *tasks, const char *status, const char *events, bool question) {
    char *response = xstrfmt("{\"session\":{\"id\":\"background-test\",\"status\":\"%s\",\"awaitingAnswer\":%s,\"backgroundTasks\":%s},\"events\":%s}",
        status, question ? "true" : "false", tasks, events);
    f->read_response = response;
    screen_conversation_refresh((ConversationScreen *)screen, false); screen_fixture_pump(f, 1);
    f->read_response = NULL; free(response);
}
static Item *working_item(Doc *doc) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].id == 2) return &doc->items[i];
    return NULL;
}
static void background_poll_lifecycle(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, 1);
    wchar_t temp[MAX_PATH], directory[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp) != 0);
    CHECK(GetTempFileNameW(temp, L"bgw", 0, directory) != 0); DeleteFileW(directory);
    CHECK(CreateDirectoryW(directory, NULL)); g_store.cache = cache_new(directory);
    Screen *screen = open_conversation(&f); Doc doc; doc_init(&doc);
    const char *bash = "[{\"name\":\"Bash\",\"summary\":\"watch CI\"}]";
    refresh(&f, screen, bash, "running", "[{\"seq\":1,\"kind\":\"text\",\"t\":\"2026-10-27T10:00:00Z\",\"text\":\"Launching\"}]", false);
    screen_fixture_draw(screen, &doc); Item *item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Working\xE2\x80\xA6");
    refresh(&f, screen, bash, "running", "[{\"seq\":2,\"kind\":\"result\"},{\"seq\":3,\"kind\":\"info\",\"text\":\"PR open\"},{\"seq\":4,\"kind\":\"stderr\",\"text\":\"warning\"}]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Waiting on Bash: watch CI\xE2\x80\xA6");
    ConversationScreen *s = (ConversationScreen *)screen; CHECK_INT(s->transcript.cursor, 4);
    // A session-only poll must update the displayed work without adding any transcript lines.
    refresh(&f, screen, "[{\"name\":\"Monitor\"},{\"name\":\"Explore\"}]", "running", "[]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Waiting on 2 background tasks\xE2\x80\xA6");
    CHECK_INT(s->transcript.count, 4); CHECK_INT(s->transcript.cursor, 4);
    refresh(&f, screen, "[]", "running", "[]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Working\xE2\x80\xA6");
    refresh(&f, screen, bash, "running", "[{\"seq\":5,\"kind\":\"user\",\"text\":\"Next\"}]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Working\xE2\x80\xA6");
    refresh(&f, screen, bash, "running", "[{\"seq\":6,\"kind\":\"ask\",\"question\":\"Proceed?\"}]", true);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    refresh(&f, screen, bash, "closed", "[]", false);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    // Destruction and a different screen cannot retain the old snapshot or indicator.
    doc_free(&doc); screen->vt->destroy(screen);
    screen = open_conversation(&f);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK_STR(item->tip, "Working\xE2\x80\xA6");
    // Restoring the saved transcript still uses the newly read session's task list.
    refresh(&f, screen, "null", "idle", "[]", false);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    CHECK_INT(((ConversationScreen *)screen)->transcript.cursor, 6);
    doc_free(&doc); screen->vt->destroy(screen);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL;
    RemoveDirectoryW(directory); screen_fixture_clear(&f);
}
void app_conversation_tests(void) {
    screen_fixture_prepare();
    test_run("conversation polls replace background work with unchanged transcript and teardown", background_poll_lifecycle);
}
