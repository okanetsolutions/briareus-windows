#include "screen_fixture.h"
#include "screen_conversation_internal.h"
#include "suites.h"

static Route ROUTES[] = {{"GET", "/sessions/{sessionId}", "read"}};

static Screen *open_conversation(ScreenFixture *f) {
    Session session = {0}; session.raw = json_parsez("{\"id\":\"background-test\",\"status\":\"running\"}");
    Screen *screen = conversation_screen_new(&session); session_free(&session);
    if (!f->pane) screen_fixture_pane(f, screen); else screen->pane = f->pane;
    screen->vt->visible(screen, true);
    CHECK(((ConversationScreen *)screen)->composer != NULL);
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
static void check_working_tooltip(const Item *item) {
    CHECK(item->tip && item->tip[0]);
    if (item->tip) {
        CHECK(strstr(item->tip, "Bash") == NULL);
        CHECK(strstr(item->tip, "watch CI") == NULL);
        CHECK(strstr(item->tip, "2") == NULL);
    }
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
    if (item) check_working_tooltip(item);
    refresh(&f, screen, bash, "running", "[{\"seq\":2,\"kind\":\"result\"},{\"seq\":3,\"kind\":\"info\",\"text\":\"PR open\"},{\"seq\":4,\"kind\":\"stderr\",\"text\":\"warning\"}]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) {
        CHECK(item->tip && item->tip[0]);
        if (item->tip) {
            CHECK(strstr(item->tip, "Bash") != NULL);
            CHECK(strstr(item->tip, "watch CI") != NULL);
        }
        int hit = doc_hit(&doc, item->rc.left + 1, item->rc.top + 1);
        CHECK(hit >= 0);
        if (hit >= 0) { CHECK(doc_item(&doc, hit) == item); CHECK_INT(doc_item(&doc, hit)->action, ACTION_TIP); }
    }
    ConversationScreen *s = (ConversationScreen *)screen; CHECK_INT(s->transcript.cursor, 4);
    // A session-only poll must update the displayed work without adding any transcript lines.
    refresh(&f, screen, "[{\"name\":\"Monitor\"},{\"name\":\"Explore\"}]", "running", "[]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) CHECK(item->tip && strstr(item->tip, "2") != NULL);
    CHECK_INT(s->transcript.count, 4); CHECK_INT(s->transcript.cursor, 4);
    refresh(&f, screen, "[]", "running", "[]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) check_working_tooltip(item);
    refresh(&f, screen, bash, "running", "[{\"seq\":5,\"kind\":\"user\",\"text\":\"Next\"}]", false);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) check_working_tooltip(item);
    refresh(&f, screen, bash, "running", "[{\"seq\":6,\"kind\":\"ask\",\"question\":\"Proceed?\"}]", true);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    refresh(&f, screen, bash, "closed", "[]", false);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    // Destruction and a different screen cannot retain the old snapshot or indicator.
    doc_free(&doc); screen->vt->destroy(screen);
    screen = open_conversation(&f);
    screen_fixture_draw(screen, &doc); item = working_item(&doc); CHECK(item != NULL);
    if (item) check_working_tooltip(item);
    // Restoring the saved transcript still uses the newly read session's task list.
    refresh(&f, screen, "null", "idle", "[]", false);
    screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
    CHECK_INT(((ConversationScreen *)screen)->transcript.cursor, 6);
    doc_free(&doc); screen->vt->destroy(screen);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL;
    RemoveDirectoryW(directory); screen_fixture_clear(&f);
}
static void deleted_review_stops_polling_preserves_pr_navigation(void) {
    for (int review = 0; review < 2; review++) {
        ScreenFixture f; screen_fixture_init(&f, ROUTES, 1);
        wchar_t temp[MAX_PATH], directory[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, temp));
        CHECK(GetTempFileNameW(temp, L"rmd", 0, directory)); DeleteFileW(directory);
        CHECK(CreateDirectoryW(directory, NULL)); g_store.cache = cache_new(directory);
        Session initial = {0}; initial.raw = json_parsez("{\"id\":\"ephemeral\",\"status\":\"running\",\"activity\":\"review\",\"repo\":\"o/r\",\"startedOnPr\":12}");
        if (!review) json_set_str(initial.raw, "activity", "chat");
        Screen *screen = conversation_screen_new(&initial); session_free(&initial); screen_fixture_pane(&f, screen);
        ConversationScreen *s = (ConversationScreen *)screen; Doc doc; doc_init(&doc);
        f.read_status = 404; f.read_response = "{\"error\":\"Session not found\"}";
        screen_conversation_refresh(s, false); screen_fixture_pump(&f, 1);
        CHECK(s->loaded); CHECK(!s->loading); CHECK_INT(s->removed, review);
        if (review) {
            CHECK(s->error == NULL); CHECK(!session_is_active(screen_conversation_session(s))); CHECK(!s->poller.running);
            screen_fixture_draw(screen, &doc); CHECK(working_item(&doc) == NULL);
            CHECK(!screen_conversation_can_message(s));
            screen_conversation_refresh(s, true); CHECK(s->req_refresh == NULL); CHECK_INT(f.reads, 1);
            screen_conversation_mutate(s, "reopen", NULL); CHECK(s->req_mutate == NULL); CHECK_INT(f.writes, 0);
            HeaderInfo info = {0}; screen->vt->header(screen, &info); CHECK_INT(info.button_count, 1);
            CHECK_INT(info.buttons[0].action, ACT_MENU_ITEM + MENU_PULL); CHECK(info.buttons[0].enabled);
        } else { CHECK(s->error != NULL); CHECK(session_is_active(screen_conversation_session(s))); }
        doc_free(&doc); screen->vt->destroy(screen);
        cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL;
        RemoveDirectoryW(directory); screen_fixture_clear(&f);
    }
}

void app_conversation_tests(void) {
    screen_fixture_prepare();
    test_run("deleted review stops polling without claiming verdict and keeps PR navigation", deleted_review_stops_polling_preserves_pr_navigation);
    test_run("conversation polls replace background work with unchanged transcript and teardown", background_poll_lifecycle);
}
