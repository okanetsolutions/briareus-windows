// Repository credentials are resolved by the server; its diagnostic must survive each screen's read/write path.
#include "screen_pulls_internal.h"
#include "project_files_internal.h"
// Both family headers name an action A_OPEN_GITHUB; only BoardTab state is used here.
#define A_OPEN_GITHUB BOARD_OPEN_GITHUB
#include "project_board_internal.h"
#undef A_OPEN_GITHUB
#include "screen_fixture.h"
#include "suites.h"

static const char *errors[] = {
    "no GitHub token for o/r",
    "GitHub account registry is unavailable; initialize it before use"
};
static wchar_t cache_dir[MAX_PATH];
static const Project project = { .repo = "o/r", .has_board = true };

static void setup(ScreenFixture *f, Route *routes, size_t count, const char *message) {
    screen_fixture_init(f, routes, count);
    g_store.cache = cache_new(cache_dir); cache_remove_all(g_store.cache);
    Json *body = json_object(); json_set_str(body, "error", message);
    f->read_status = 503; f->read_response = json_serialize(body, false); json_free(body);
}
static void cleanup(ScreenFixture *f) {
    free((char *)f->read_response);
    cache_remove_all(g_store.cache); cache_free(g_store.cache); RemoveDirectoryW(cache_dir);
    screen_fixture_clear(f);
}
// Drain cancelled completions as well as live requests before inspecting worker counters.
static void settle(ScreenFixture *f) {
    ULONGLONG end = GetTickCount64() + 400;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, f->dispatcher, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE))
            store_handle_message(msg.message, msg.wParam, msg.lParam);
        Sleep(1);
    }
}
static bool contains(const Doc *doc, const char *message) {
    for (size_t i = 0; i < doc->count; i++)
        if (doc->items[i].text && strstr(doc->items[i].text, message)) return true;
    return false;
}
static void board_errors_wait_for_explicit_refresh(void) {
    Route routes[] = {{"GET", "/project-board", "read"}};
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        ScreenFixture f; setup(&f, routes, 1, errors[i]);
        Screen host = {0}; BoardTab *board = board_tab_new(&project, &host, 1000);
        board_tab_open(board); screen_fixture_pump(&f, 1);
        Doc doc; doc_init(&doc);
        for (int frame = 0; frame < 20; frame++) {
            doc_begin(&doc, NULL, 900); board_tab_layout(board, &doc, 900);
            CHECK(contains(&doc, errors[i]));
        }
        CHECK(board->req == NULL);
        settle(&f); CHECK_INT(f.reads, 1);
        board_tab_refresh(board); screen_fixture_pump(&f, 1);
        doc_begin(&doc, NULL, 900); board_tab_layout(board, &doc, 900);
        CHECK(contains(&doc, errors[i])); CHECK_INT(f.reads, 2);
        doc_free(&doc); board_tab_free(board); cleanup(&f);
    }
}
static void files_tree_and_file_errors_remain_visible(void) {
    Route routes[] = {{"GET", "/repo/tree", "read"}, {"GET", "/repo/file", "read"}};
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        ScreenFixture f; setup(&f, routes, 2, errors[i]);
        Screen host = {0}; ProjectFiles *files = project_files_new("o/r", &host, 1000, 9);
        project_files_open(files); screen_fixture_pump(&f, 1);
        Doc doc; doc_init(&doc);
        doc_begin(&doc, NULL, 900); project_files_layout(files, &doc, 900);
        CHECK(contains(&doc, errors[i])); CHECK_INT(f.reads, 1);
        const char *failure = f.read_response;
        // Omit sha deliberately: indexing would retain the cache past this fixture's cleanup.
        f.read_status = 200; f.read_response = "{\"ref\":\"main\",\"entries\":[]}";
        project_files_refresh(files); screen_fixture_pump(&f, 1); CHECK(files->has_tree);
        f.read_status = 503; f.read_response = failure;
        project_files_open_path(files, "README.md"); screen_fixture_pump(&f, 1);
        doc_begin(&doc, NULL, 900); project_files_layout(files, &doc, 900);
        CHECK(contains(&doc, errors[i])); CHECK_INT(f.reads, 3);
        doc_free(&doc); project_files_free(files); cleanup(&f);
    }
}
static void issue_and_pull_details_and_pull_edits_show_server_errors(void) {
    Route routes[] = {{"GET", "/issues/{number}", "read"}, {"GET", "/pulls/{pr}", "read"},
        {"PATCH", "/pulls/{pr}", "manage"}};
    for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
        ScreenFixture f; setup(&f, routes, 3, errors[i]);
        IssueSummary summary = { .number = 1 };
        Screen *issue = issue_detail_screen_new(&project, &summary); screen_fixture_pane(&f, issue);
        screen_pulls_issue_load((IssueScreen *)issue, false); screen_fixture_pump(&f, 1);
        Doc doc; doc_init(&doc); screen_fixture_draw(issue, &doc); CHECK(contains(&doc, errors[i]));
        issue->vt->destroy(issue);
        Screen *pull = pull_detail_screen_new(&project, 1, NULL, NULL); pull->pane = f.pane;
        screen_pulls_pull_load((PullScreen *)pull); screen_fixture_pump(&f, 1);
        screen_fixture_draw(pull, &doc); CHECK(contains(&doc, errors[i]));
        f.write_status = 503; f.write_response = f.read_response;
        Json *fields = json_object(); json_set_str(fields, "title", "New title");
        CHECK(store_supports("update_pull"));
        screen_pulls_pull_edit((PullScreen *)pull, fields); screen_fixture_pump(&f, 2);
        settle(&f); CHECK_INT(f.writes, 1); CHECK(!((PullScreen *)pull)->editing);
        CHECK(((PullScreen *)pull)->edit_error && strstr(((PullScreen *)pull)->edit_error, errors[i]));
        set_string(&((PullScreen *)pull)->error, NULL);
        screen_fixture_draw(pull, &doc); CHECK(contains(&doc, errors[i]));
        doc_free(&doc); pull->vt->destroy(pull); cleanup(&f);
    }
}
void app_repository_errors_tests(void) {
    screen_fixture_prepare();
    GetTempPathW(MAX_PATH, cache_dir); wcscat(cache_dir, L"briareus-repository-errors-tests");
    test_run("Board credential failures surface the server error and wait for refresh", board_errors_wait_for_explicit_refresh);
    test_run("Files tree and file credential failures surface the server error", files_tree_and_file_errors_remain_visible);
    test_run("Issue and PR details and PR edits surface credential errors without retrying writes", issue_and_pull_details_and_pull_edits_show_server_errors);
}
