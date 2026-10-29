#include "action_runtime.h"
#include "screen_fixture.h"
#include "screen_pulls_internal.h"
#include "modal_driver.h"
#include "resource.h"
#include <commctrl.h>
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/runtimes", "read"}, {"GET", "/providers", "admin"},
    {"POST", "/actions", "manage"}, {"GET", "/actions", "read"}, {"POST", "/sessions", "manage"}
};
static const char *RUNTIMES = "{\"default\":{\"providerId\":1,\"model\":\"gpt\",\"effort\":\"high\"},\"providers\":["
    "{\"id\":1,\"label\":\"Claude label is not identity\",\"available\":true,\"models\":[{\"id\":\"gpt\"}]},"
    "{\"id\":2,\"label\":\"Custom account\",\"available\":true,\"defaultModel\":\"opus\",\"models\":["
    "{\"id\":\"opus\",\"efforts\":[\"low\",\"high\"],\"defaultEffort\":\"high\"},"
    "{\"id\":\"sonnet\",\"efforts\":[\"medium\"],\"defaultEffort\":\"medium\"}]}]}";
static const char *PROVIDERS = "{\"providers\":[{\"id\":1,\"binary\":\"codex\"},{\"id\":2,\"binary\":\"claude\",\"available\":true,\"auth\":{\"loggedIn\":true}}]}";
static const char *CATALOG = "[{\"id\":\"review\",\"label\":\"Code review\",\"binary\":\"claude\"}]";
static const char *runtime_reply, *provider_reply;
static wchar_t cache_directory[MAX_PATH];
static bool transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    ScreenFixture *f = ctx;
    if (strstr(url, "/runtimes?")) f->read_response = runtime_reply;
    else if (strstr(url, "/providers")) f->read_response = provider_reply;
    return screen_fixture_transport(ctx, method, url, headers, body, bytes, timeout, status, type, retry, response, len, error);
}
static void setup(ScreenFixture *f) {
    screen_fixture_init(f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    wchar_t temp[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"acr", 0, cache_directory)); DeleteFileW(cache_directory);
    CHECK(CreateDirectoryW(cache_directory, NULL)); g_store.cache = cache_new(cache_directory);
    runtime_reply = RUNTIMES; provider_reply = PROVIDERS;
    f->write_status = 201; f->write_response = "{\"session\":{\"id\":\"review-fixture\",\"status\":\"preparing\",\"activity\":\"review\"}}";
    api_client_set_transport(f->client, transport, f);
}
static void cleanup(ScreenFixture *f) {
    cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL; RemoveDirectoryW(cache_directory);
    screen_fixture_clear(f);
}
static void load(ScreenFixture *f, ActionRuntime *s) {
    action_runtime_init(s, "o/r", "claude", NULL, NULL); action_runtime_load(s); screen_fixture_pump(f, 2);
}
static void exact_payload(ScreenFixture *f) {
    CHECK_STR(f->method, "POST"); CHECK_STR(f->url, "https://example.com/api/v1/actions");
    CHECK_INT(json_count(f->body), 6); CHECK_STR(json_str(json_get(f->body, "repo")), "o/r");
    CHECK_STR(json_str(json_get(f->body, "action")), "review"); CHECK_INT(json_int_or(json_get(f->body, "prNumber"), 0), 12);
    CHECK_INT(json_int_or(json_get(f->body, "provider"), 0), 2); CHECK_STR(json_str(json_get(f->body, "model")), "sonnet");
    CHECK_STR(json_str(json_get(f->body, "effort")), "medium"); CHECK(json_is_null(json_get(f->body, "providerId")));
}
static void valid_selection_and_both_submit_paths(void) {
    ScreenFixture f; setup(&f); ActionRuntime runtime; load(&f, &runtime);
    CHECK(runtime.ready); CHECK_INT(runtime.choice.provider_id, 2); CHECK_STR(runtime.choice.model, "opus"); CHECK_STR(runtime.choice.effort, "high");
    CHECK(!action_runtime_select(&runtime, 1, "gpt", NULL)); CHECK(!action_runtime_select(&runtime, 2, "gpt", NULL));
    CHECK(!action_runtime_select(&runtime, 2, "opus", "max"));
    CHECK(action_runtime_select(&runtime, 2, "opus", "low")); CHECK_STR(runtime.choice.effort, "low");
    CHECK(action_runtime_select(&runtime, 2, "sonnet", NULL)); CHECK_STR(runtime.choice.effort, "medium");
    RuntimeChoice chosen = {0}; CHECK(action_runtime_take(&runtime, &chosen));
    Json *catalog = json_parsez(CATALOG); size_t n; BoardAction *actions = screen_pulls_row_actions(catalog, NULL, 0, &n);
    const BoardAction *review = NULL; for (size_t i = 0; i < n; i++) if (str_eq(actions[i].id, "review")) review = &actions[i]; CHECK(review != NULL);
    Project project = {.repo = "o/r"}; Screen *screen = pulls_screen_new(&project); screen_fixture_pane(&f, screen);
    PullsScreen *board = (PullsScreen *)screen;
    screen_pulls_board_start(board, 12, "topic", review, NULL, &chosen);
    screen_pulls_board_start(board, 12, "topic", review, NULL, &chosen); // duplicate submit
    screen_fixture_pump(&f, 1); CHECK_INT(f.writes, 1); CHECK(!board->busy); exact_payload(&f);
    screen->vt->destroy(screen);
    screen = pull_detail_screen_new(&project, 12, NULL, NULL); screen->pane = f.pane;
    PullScreen *pull = (PullScreen *)screen; json_free(pull->pr); pull->pr = json_parsez("{\"headRef\":\"topic\"}");
    screen_pulls_start_action(pull, review, NULL, &chosen); screen_pulls_start_action(pull, review, NULL, &chosen);
    screen_fixture_pump(&f, 1); CHECK_INT(f.writes, 2); CHECK(!pull->busy); exact_payload(&f);
    screen_pulls_start_action(pull, review, NULL, NULL); CHECK(!pull->busy); CHECK_INT(f.writes, 2);
    g_store.device.permission = "read"; screen_pulls_start_action(pull, review, NULL, &chosen); CHECK_INT(f.writes, 2);
    screen->vt->destroy(screen); runtime_choice_free(&chosen); action_runtime_free(&runtime);
    board_actions_free(actions, n); json_free(catalog); cleanup(&f);
}
static void compatible_default_and_manage_identity(void) {
    ScreenFixture f; setup(&f);
    runtime_reply = "{\"default\":{\"providerId\":2,\"model\":\"opus\",\"effort\":\"low\"},\"providers\":[{\"id\":2,\"label\":\"Account\",\"binary\":\"claude\",\"models\":[{\"id\":\"opus\",\"efforts\":[\"low\",\"high\"],\"defaultEffort\":\"high\"}]}]}";
    char *repos[] = {"o/r"}; g_store.device.permission = "manage"; g_store.device.repos = repos; g_store.device.repo_count = 1;
    ActionRuntime runtime; action_runtime_init(&runtime, "o/r", "claude", NULL, NULL); action_runtime_load(&runtime); screen_fixture_pump(&f, 1);
    CHECK(runtime.ready); CHECK_STR(runtime.choice.effort, "low"); CHECK_INT(f.reads, 1);
    RuntimeChoice chosen = {0}; CHECK(action_runtime_take(&runtime, &chosen)); runtime_choice_free(&chosen);
    g_store.device.repos = NULL; g_store.device.repo_count = 0;
    CHECK(!action_runtime_take(&runtime, &chosen)); action_runtime_free(&runtime); cleanup(&f);
}
static void unavailable_and_denied_never_start(void) {
    ScreenFixture f; setup(&f); ActionRuntime runtime;
    provider_reply = "{\"providers\":[{\"id\":1,\"binary\":\"codex\"},{\"id\":2,\"binary\":\"claude\",\"auth\":{\"loggedIn\":false}}]}";
    load(&f, &runtime); CHECK(!runtime.ready); CHECK(runtime.error != NULL); action_runtime_free(&runtime);
    char *repos[] = {"o/r"}; g_store.device.permission = "manage"; g_store.device.repos = repos; g_store.device.repo_count = 1;
    action_runtime_init(&runtime, "o/r", "claude", NULL, NULL); action_runtime_load(&runtime); screen_fixture_pump(&f, 1);
    CHECK(!runtime.ready); CHECK(runtime.error != NULL); CHECK_INT(f.reads, 3); action_runtime_free(&runtime);
    g_store.device.permission = "read"; action_runtime_init(&runtime, "o/r", "claude", NULL, NULL); action_runtime_load(&runtime);
    CHECK(runtime.read == NULL); CHECK(runtime.error != NULL); action_runtime_free(&runtime);
    CHECK_INT(f.writes, 0); cleanup(&f);
}
static void cancellation_and_stale_context(void) {
    for (int change = 0; change < 4; change++) {
        ScreenFixture f; setup(&f); f.block_read = true; ActionRuntime runtime;
        action_runtime_init(&runtime, "o/r", "claude", NULL, NULL); action_runtime_load(&runtime);
        CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
        ApiClient *original = g_store.client;
        if (change == 0) action_runtime_free(&runtime);
        if (change == 1) g_store.client = NULL;
        if (change == 2) g_store.device.id = "another-token";
        if (change == 3) g_store.device.permission = "read";
        SetEvent(f.resume); screen_fixture_pump(&f, 1);
        if (change) {
            RuntimeChoice choice = {0}; CHECK(!runtime.ready); CHECK(!action_runtime_take(&runtime, &choice)); action_runtime_free(&runtime);
        }
        CHECK_INT(f.writes, 0); CHECK_INT(f.reads, 1); g_store.client = original; cleanup(&f);
    }
}
static void catalog_absence_does_not_use_retired_review_route(void) {
    ScreenFixture f; setup(&f); size_t n;
    BoardAction *actions = screen_pulls_row_actions(NULL, NULL, 0, &n);
    CHECK_INT(n, 0); board_actions_free(actions, n); // /sessions must not imply support for review
    Json *catalog = json_parsez("[]"); actions = screen_pulls_row_actions(catalog, NULL, 0, &n);
    CHECK_INT(n, 0); board_actions_free(actions, n); json_free(catalog); cleanup(&f);
}
static void obsolete_action_catalog_cannot_replace_current_metadata(void) {
    ScreenFixture f; setup(&f); f.block_read = true;
    f.read_response = "{\"actions\":[{\"id\":\"review\",\"label\":\"Old catalog\",\"binary\":\"other-cli\"}]}";
    Project project = {.repo = "o/r"}; Screen *screen = pull_detail_screen_new(&project, 12, NULL, NULL);
    screen_fixture_pane(&f, screen); PullScreen *pull = (PullScreen *)screen;
    screen_pulls_pull_load(pull); CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0);
    json_free(pull->catalog); pull->catalog = json_parsez(CATALOG);
    // Complete the unsupported pull read first, then replace the connection before the held action catalog returns.
    screen_fixture_pump(&f, 1); ApiClient *original = g_store.client; g_store.client = NULL;
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_STR(json_str(json_get(json_at(pull->catalog, 0), "binary")), "claude");
    Json *saved = cache_value(g_store.cache, "actions"); CHECK(saved == NULL); json_free(saved);
    CHECK_INT(f.writes, 0); g_store.client = original; screen->vt->destroy(screen); cleanup(&f);
}

static void stale_action_completions_are_isolated(void) {
    for (int view = 0; view < 2; view++) for (int change = 0; change < 3; change++) for (int fail = 0; fail < 2; fail++) {
        ScreenFixture f; setup(&f); f.block_write = true; f.fail_write = fail != 0;
        Project project = {.repo = "o/r"};
        Screen *screen = view ? pull_detail_screen_new(&project, 12, NULL, NULL) : pulls_screen_new(&project);
        screen_fixture_pane(&f, screen);
        BoardAction review = {.id = "review", .binary = "claude", .advertised = true}; RuntimeChoice chosen = {2, "sonnet", "medium"};
        PullScreen *pull = view ? (PullScreen *)screen : NULL; PullsScreen *board = view ? NULL : (PullsScreen *)screen;
        char **write_error = view ? &pull->write_error : &board->write_error; set_string(write_error, "current-context-error");
        if (view) { json_free(pull->pr); pull->pr = json_parsez("{\"headRef\":\"topic\"}"); screen_pulls_start_action(pull, &review, NULL, &chosen); }
        else screen_pulls_board_start(board, 12, "topic", &review, NULL, &chosen);
        CHECK_INT(WaitForSingleObject(f.entered, 5000), WAIT_OBJECT_0); ApiClient *original = g_store.client;
        if (change == 0) g_store.client = NULL;
        if (change == 1) g_store.device.id = "different-account";
        if (change == 2) set_string(view ? &pull->project.repo : &board->project.repo, "another/project");
        SetEvent(f.resume); screen_fixture_pump(&f, 1);
        CHECK_STR(*write_error, "current-context-error"); CHECK(!(view ? pull->uncertain : board->uncertain));
        CHECK(!(view ? pull->busy : board->busy)); CHECK_INT(f.reads, 0); CHECK_INT(f.writes, 1);
        g_store.client = original; screen->vt->destroy(screen); cleanup(&f);
    }
}

static ScreenFixture *modal_fixture;
static Screen *modal_source;
static RuntimeChoice modal_choice;
static bool modal_ok;
static int modal_scenario;
static HWND modal_window;
static BOOL CALLBACK find_modal(HWND window, LPARAM arg) {
    (void)arg; wchar_t name[32]; GetClassNameW(window, name, 32);
    if (!wcscmp(name, L"#32770") && IsWindowVisible(window)) { modal_window = window; return FALSE; }
    return TRUE;
}
static void prompt_driver(Screen *unused, int action, intptr_t arg, POINT pt) {
    (void)unused; (void)action; (void)arg; (void)pt;
    BoardAction review = {.id = "review", .label = "Code review", .binary = "claude", .advertised = true};
    char *input = NULL; modal_ok = screen_pulls_action_prompt(modal_source, "o/r", &review, 12, &input, &modal_choice); free(input);
}
static void during_modal(void) {
    CHECK_INT(WaitForSingleObject(modal_fixture->entered, 5000), WAIT_OBJECT_0);
    if (modal_scenario == 4) return; // Close while the catalog worker is held.
    SetEvent(modal_fixture->resume); screen_fixture_pump(modal_fixture, 2);
    modal_window = NULL; EnumThreadWindows(GetCurrentThreadId(), find_modal, 0); CHECK(modal_window != NULL);
    CHECK_INT(SendDlgItemMessageW(modal_window, IDC_MODEL, CB_GETCOUNT, 0, 0), 2);
    CHECK(IsWindowEnabled(GetDlgItem(modal_window, IDC_START)));
    SendDlgItemMessageW(modal_window, IDC_MODEL, CB_SETCURSEL, 1, 0);
    SendMessageW(modal_window, WM_COMMAND, MAKEWPARAM(IDC_MODEL, CBN_SELCHANGE), 0);
    CHECK_INT(SendDlgItemMessageW(modal_window, IDC_EFFORT, CB_GETCOUNT, 0, 0), 1);
    if (modal_scenario == 2) pane_set_root(modal_fixture->pane, NULL);
    if (modal_scenario == 3) g_store.device.id = "changed-during-popup";
}
static void runtime_modal_selection_cancel_and_owner_replacement(void) {
    static const ScreenVTable owner_vt = {.destroy = screen_release};
    static const ScreenVTable driver_vt = {.action = prompt_driver};
    for (modal_scenario = 0; modal_scenario < 5; modal_scenario++) {
        ScreenFixture f; setup(&f); modal_fixture = &f; f.block_read = true; modal_choice = (RuntimeChoice){0}; modal_ok = false;
        modal_source = xcalloc(1, sizeof *modal_source); modal_source->vt = &owner_vt;
        screen_fixture_pane(&f, modal_source); pane_set_root(f.pane, modal_source);
        Screen driver = {.vt = &driver_vt};
        int answer = modal_scenario == 1 || modal_scenario == 4 ? IDCANCEL : IDC_START;
        modal_driver_action_during(&driver, 0, 0, answer, during_modal);
        CHECK_INT(modal_ok, modal_scenario == 0);
        if (modal_ok) { CHECK_INT(modal_choice.provider_id, 2); CHECK_STR(modal_choice.model, "sonnet"); CHECK_STR(modal_choice.effort, "medium"); }
        if (modal_scenario == 4) { SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 1); }
        runtime_choice_free(&modal_choice); CHECK_INT(f.writes, 0); cleanup(&f);
    }
}

void app_action_runtime_tests(void) {
    screen_fixture_prepare();
    test_run("old endpoint action catalog cannot overwrite current metadata or cache", obsolete_action_catalog_cannot_replace_current_metadata);
    test_run("action POST completions ignore replaced endpoint account and project", stale_action_completions_are_isolated);
    test_run("runtime popup selects model, cancels and rejects changed owners or tokens", runtime_modal_selection_cancel_and_owner_replacement);
    test_run("review selects valid Claude runtime and submits exact action once from both views", valid_selection_and_both_submit_paths);
    test_run("review honors compatible project default and manage runtime metadata", compatible_default_and_manage_identity);
    test_run("review rejects unavailable runtime and denied identity access without writes", unavailable_and_denied_never_start);
    test_run("review runtime cancellation and stale connection token permission cannot submit", cancellation_and_stale_context);
    test_run("sessions capability cannot resurrect retired review trigger", catalog_absence_does_not_use_retired_review_route);
}
