#include "attachment_fixture.h"
#include "saved_prompts_internal.h"
#include "screen_new_session_internal.h"
#include "suites.h"

static void initial_prompt_gates_and_project_changes(void) {
    UploadFixture f; init(&f); wchar_t path[MAX_PATH]; temp(path); write_file(path, "file"); const wchar_t *p = path;
    Project projects[] = {{.repo = "o/first"}, {.repo = "o/next"}};
    NewSessionScreen s = {0}; s.projects = projects; s.count = 2;
    screen_fixture_pane(&f.screen, &s.base);
    s.composer = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_MULTILINE, 0, 0, 100, 20, f.screen.parent, NULL, GetModuleHandleW(NULL), NULL);
    s.files = attach_list_new(&s.base, "start_session"); screen_new_session_load_choices(&s);
    attach_list_paths(s.files, &p, 1); CHECK(!screen_new_session_can_start(&s));
    screen_new_session_start(&s); CHECK(!s.busy);
    pump(&f, WM_APP_ASYNC_DONE); CHECK(!screen_new_session_can_start(&s)); complete_file(&f);
    CHECK(screen_new_session_can_start(&s)); CHECK_INT(f.calls, 1);
    api_client_set_transport(f.screen.client, screen_fixture_transport, &f.screen);
    screen_new_session_start(&s); CHECK(s.busy); CHECK(s.req_start != NULL);
    if (s.req_start) {
        CHECK_STR(json_str(json_at(json_get(s.req_start->args, "attachments"), 0)), "file-1");
        CHECK_STR(json_str(json_get(s.req_start->args, "repo")), "o/first");
    }
    request_cancel(&s.req_start); pump(&f, WM_APP_REQUEST_DONE); s.busy = false; CHECK_INT(f.screen.writes, 1);
    s.chosen = 1; screen_new_session_load_choices(&s); CHECK_INT(attach_list_count(s.files), 0); CHECK(!screen_new_session_can_start(&s));
    attach_list_paths(s.files, &p, 1); s.chosen = 0; screen_new_session_load_choices(&s);
    pump(&f, WM_APP_ASYNC_DONE); CHECK_INT(attach_list_count(s.files), 0); CHECK_INT(f.calls, 1);
    attach_list_free(s.files); DestroyWindow(s.composer); DeleteFileW(path); screen_fixture_clear(&f.screen);
}

static void initial_prompt_library_follows_project(void) {
    static Route prompt_routes[] = {
        {"GET", "/prompts", "read"}, {"POST", "/prompts", "admin"}, {"POST", "/sessions", "manage"},
    };
    ScreenFixture f;
    screen_fixture_init(&f, prompt_routes, sizeof prompt_routes / sizeof *prompt_routes);
    g_store.device.permission = "manage";
    char *repos[] = {"o/first", "o/next"};
    g_store.device.repos = repos;
    g_store.device.repo_count = sizeof repos / sizeof *repos;
    f.read_response = "{\"prompts\":["
        "{\"id\":1,\"title\":\"First\",\"body\":\"saved\\r\\nbody\",\"repo\":\"o/first\",\"sortOrder\":1},"
        "{\"id\":2,\"title\":\"Shared\",\"body\":\"shared\",\"sortOrder\":2}]}";
    Project projects[] = {{.repo = "o/first"}, {.repo = "o/next"}};
    Screen *base = new_session_screen_new(&projects[0], projects, 2);
    NewSessionScreen *s = (NewSessionScreen *)base;
    screen_fixture_pane(&f, base);
    pane_set_root(f.pane, base);
    RECT footer = {0, 400, 900, 600};
    base->vt->footer_paint(base, NULL, &footer);
    CHECK(!IsRectEmpty(&s->prompts_rc));
    CHECK(!prompt_picker_writable(s->prompts, "create_prompt"));

    prompt_picker_read(s->prompts);
    CHECK_STR(json_str(json_get(s->prompts->read ? s->prompts->read->args : NULL, "repo")), "o/first");
    screen_fixture_pump(&f, 1);
    CHECK_INT(json_count(s->prompts->rows), 2);
    screen_new_session_set_composer_text(s, "written text");
    SendMessageW(s->composer, EM_SETSEL, 0, -1);
    prompt_picker_insert_row(s->prompts, saved_prompt_find(s->prompts->rows, 1));
    CHECK_OWNED_STR(screen_new_session_composer_text(s), "written text\n\nsaved\nbody");
    CHECK(!s->req_start);

    // An in-flight read of the first project must not populate the next project's menu.
    f.block_read = true;
    prompt_picker_read(s->prompts);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    Screen *fresh = new_session_screen_new(&projects[1], projects, 2);
    base->vt->adopt(base, fresh);
    fresh->vt->destroy(fresh);
    SetEvent(f.resume);
    screen_fixture_pump(&f, 1);
    CHECK(!s->prompts->loaded);
    CHECK(s->prompts->rows == NULL);
    CHECK_OWNED_STR(screen_new_session_composer_text(s), "written text\n\nsaved\nbody");

    prompt_picker_read(s->prompts);
    CHECK_STR(json_str(json_get(s->prompts->read ? s->prompts->read->args : NULL, "repo")), "o/next");
    screen_fixture_pump(&f, 1);
    CHECK_INT(json_count(s->prompts->rows), 1);
    prompt_picker_insert_row(s->prompts, saved_prompt_find(s->prompts->rows, 2));
    CHECK_OWNED_STR(screen_new_session_composer_text(s), "written text\n\nsaved\nbody\n\nshared");

    g_store.route_count = 0;
    base->vt->footer_paint(base, NULL, &footer);
    CHECK(IsRectEmpty(&s->prompts_rc));
    CHECK_INT(f.writes, 0);
    json_free(g_store.prompt_state);
    g_store.prompt_state = NULL;
    screen_fixture_clear(&f);
}

void app_attach_initial_tests(void) {
    test_run("initial prompt gates selection and clears changed projects", initial_prompt_gates_and_project_changes);
    test_run("new session inserts saved prompts from the selected project without sending", initial_prompt_library_follows_project);
}
