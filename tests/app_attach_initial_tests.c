#include "attachment_fixture.h"
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
void app_attach_initial_tests(void) {
    test_run("initial prompt gates selection and clears changed projects", initial_prompt_gates_and_project_changes);
}
