#include "screen_fixture.h"
#include "project_ssh_internal.h"
#include "screen_projects_internal.h"
#include "suites.h"

static Route routes[] = { { "GET", "/settings/ssh/servers", "admin" } };
static const char *old_rows = "{\"servers\":[{\"id\":1,\"repo\":\"o/one\",\"label\":\"Old\"}]}";
static const char *new_rows = "{\"servers\":[{\"id\":2,\"repo\":\"o/two\",\"label\":\"New\"}]}";
static int launches;
static bool launch(void *ctx, const TermTarget *target, char **error) {
    (void)ctx; (void)error; CHECK_STR(target->host, "fake.example"); launches++; return true;
}
// Capture the response before blocking: a cancelled old read must not replace a newer answer.
static bool snapshot_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    ScreenFixture *f = ctx; char *snapshot = xstrdup(f->read_response);
    bool ok = screen_fixture_transport(ctx, method, url, headers, body, bytes, timeout, status, type, retry, response, len, error);
    if (ok) { free(*response); *response = snapshot; *len = strlen(snapshot); } else free(snapshot);
    return ok;
}
static void refresh_race(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    api_client_set_transport(f.client, snapshot_transport, &f);
    Screen host = {0}; ProjectSsh *p = project_ssh_new(NULL, &host, 700);
    f.read_response = old_rows; f.block_read = true;
    project_ssh_load(p); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    Request *old = p->req;
    f.block_read = false; f.read_response = new_rows;
    servers_ssh_changed(); CHECK(old->cancelled); CHECK(p->req != old);
    screen_fixture_pump(&f, 1);
    CHECK_INT(json_count(p->rows), 1); CHECK_STR(json_str(json_get(json_at(p->rows, 0), "label")), "New");
    SetEvent(f.resume); screen_fixture_pump(&f, 1);
    CHECK_STR(json_str(json_get(json_at(p->rows, 0), "label")), "New");
    project_ssh_refresh(p); screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 3);
    f.read_response = "{}"; project_ssh_refresh(p); screen_fixture_pump(&f, 1);
    CHECK(p->error != NULL); CHECK_INT(json_count(p->rows), 1);
    f.read_response = new_rows; project_ssh_refresh(p); screen_fixture_pump(&f, 1); CHECK(p->error == NULL);
    project_ssh_free(p); screen_fixture_clear(&f);
}
static void reset_and_destroy(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1); Screen host = {0};
    ProjectSsh *p = project_ssh_new(NULL, &host, 700);
    f.read_response = old_rows; f.block_read = true;
    project_ssh_load(p); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    Request *old = p->req; set_string(&p->query, "old"); ssh_server_folder_toggle(p->folded, "o/one", "");
    g_store.device.id = "another-device"; servers_ssh_scope_changed(); CHECK(old->cancelled);
    CHECK_INT(json_count(p->rows), 0); CHECK_STR(p->query, ""); CHECK_INT(json_count(p->folded), 0);
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK_INT(json_count(p->rows), 0);
    // Same client, changed project scope must invalidate an in-flight list too.
    ResetEvent(f.entered); ResetEvent(f.resume); project_ssh_refresh(p);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0); old = p->req;
    char *repos[] = { "o/two" }; g_store.device.repos = repos; g_store.device.repo_count = 1;
    servers_ssh_scope_changed(); CHECK(old->cancelled);
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK_INT(json_count(p->rows), 0);
    // Sign-out discards old lists, then a subsequent connection can read again.
    g_store.client = NULL; servers_ssh_scope_changed(); CHECK(!project_ssh_current(p));
    g_store.client = f.client; f.block_read = false; f.read_response = new_rows;
    project_ssh_load(p); screen_fixture_pump(&f, 1); CHECK_INT(json_count(p->rows), 1);
    f.block_read = true; ResetEvent(f.entered); ResetEvent(f.resume); project_ssh_refresh(p);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0); old = p->req;
    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://replacement.example", &address));
    ApiClient *replacement = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    CHECK(replacement != NULL); server_address_free(&address); api_error_clear(&error);
    api_client_set_transport(replacement, snapshot_transport, &f);
    g_store.client = replacement; servers_ssh_scope_changed(); CHECK(old->cancelled); CHECK_INT(json_count(p->rows), 0);
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK_INT(json_count(p->rows), 0);
    ResetEvent(f.entered); ResetEvent(f.resume); project_ssh_refresh(p);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    project_ssh_free(p); api_client_release(replacement); g_store.client = f.client; SetEvent(f.resume); screen_fixture_pump(&f, 1);
    screen_fixture_clear(&f);
}
static void sessions_shared(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    Screen host = {0}; screen_fixture_pane(&f, &host);
    ProjectSsh *global = project_ssh_new(NULL, &host, 700), *project = project_ssh_new("o/one", &host, 710);
    Json *row = json_parsez("{\"id\":1,\"repo\":\"o/one\",\"label\":\"Web\",\"username\":\"deploy\",\"host\":\"fake.example\"}");
    launches = 0; term_set_launcher(launch, NULL);
    project_ssh_connect_row(global, row); CHECK_INT(term_count(), 1); CHECK_INT(launches, 1);
    Term *one = term_active(); CHECK_STR(term_group(one), "o/one");
    RECT bounds = {0, 0, 300, 200}; term_place(one, &bounds, true);
    CHECK(IsWindowVisible(term_hwnd(one)));
    Screen *candidate = ssh_servers_screen_new(); candidate->vt->destroy(candidate);
    CHECK(IsWindowVisible(term_hwnd(one)));
    project_ssh_connect_row(project, row); CHECK(term_active() == one); CHECK_INT(launches, 1);
    json_set_num(row, "id", 2); json_set_str(row, "repo", "o/two");
    project_ssh_connect_row(global, row); Term *two = term_active(); CHECK_INT(term_count(), 2);
    CHECK_OWNED_STR(project_ssh_tab_label(global, one), "Web · o/one");
    CHECK_OWNED_STR(project_ssh_tab_label(global, two), "Web · o/two");
    CHECK(project_ssh_shown_term(project) == one); CHECK(project_ssh_shown_term(global) == two);
    term_cycle_all(two, true); term_cycle(two, false); CHECK(term_active() == one);
    term_cycle_all(one, false); term_cycle(one, false); CHECK(term_active() == one);
    term_cycle_all(one, true); term_cycle(one, true); CHECK(term_active() == two);
    char *error = NULL; CHECK(term_reconnect(one, &error)); CHECK(error == NULL); CHECK_INT(launches, 3); free(error);
    project_ssh_connect_row(global, row); CHECK(term_active() == two); CHECK_INT(launches, 3);
    // A scope transition retires sessions as well as lists; the next account cannot reuse a registry id.
    g_store.device.permission = "read"; servers_ssh_scope_changed(); CHECK_INT(term_count(), 0);
    CHECK(!project_ssh_offered()); project_ssh_connect_row(global, row); CHECK_INT(launches, 3);
    project_ssh_free(global); project_ssh_free(project); json_free(row); servers_ssh_sessions_reset();
    term_set_launcher(NULL, NULL); screen_fixture_clear(&f);
}
static void navigation_gated(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 400); screen_projects_sidebar_top(&doc, 400, "ssh-servers"); doc_end(&doc);
    int found = screen_fixture_find(&doc, "❯ SSH servers"); CHECK(found >= 0); if (found >= 0) CHECK_INT(doc.items[found].action, ACT_SSH);
    doc_free(&doc); g_store.device.permission = "read";
    doc_init(&doc); doc_begin(&doc, NULL, 400); screen_projects_sidebar_top(&doc, 400, NULL); doc_end(&doc);
    CHECK_INT(screen_fixture_find(&doc, "❯ SSH servers"), -1); doc_free(&doc); screen_fixture_clear(&f);
}
static void search_control_keys_and_reset(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1); f.read_response = new_rows;
    Screen *s = ssh_servers_screen_new(); screen_fixture_pane(&f, s); pane_set_root(f.pane, s);
    screen_fixture_pump(&f, 1);
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc);
    RECT content = pane_content_rect(f.pane); s->vt->place(s, &content, 0);
    HWND edit = FindWindowExW(pane_hwnd(f.pane), NULL, L"EDIT", NULL); CHECK(edit != NULL);
    SetFocus(edit); SetWindowTextW(edit, L"missing"); CHECK(GetWindowTextLengthW(edit) > 0);
    SendMessageW(edit, WM_KEYDOWN, VK_ESCAPE, 0); CHECK_INT(GetWindowTextLengthW(edit), 0);
    CHECK(GetFocus() == pane_hwnd(f.pane));
    SetFocus(edit); SendMessageW(edit, WM_KEYDOWN, VK_TAB, 0); CHECK(GetFocus() == pane_hwnd(f.pane));
    SetFocus(edit); SendMessageW(edit, WM_KEYDOWN, VK_F5, 0); screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 2);
    SetWindowTextW(edit, L"previous account"); g_store.device.id = "replacement";
    screen_fixture_draw(s, &doc); CHECK_INT(GetWindowTextLengthW(edit), 0);
    screen_fixture_pump(&f, 1);
    doc_free(&doc); screen_fixture_clear(&f);
}
void app_ssh_servers_tests(void) {
    screen_fixture_prepare();
    // Warm terminal class/font registries and vector capacity before per-test CRT snapshots.
    ScreenFixture f; screen_fixture_init(&f, routes, 1); Screen host = {0}; screen_fixture_pane(&f, &host);
    term_set_launcher(launch, NULL); TermTarget target = {"warm", "", "warm", "deploy", "fake.example", 22}; char *error = NULL;
    Term *t = term_open(pane_hwnd(f.pane), &target, &error); CHECK(t != NULL); free(error); term_close(t);
    term_set_launcher(NULL, NULL); screen_fixture_clear(&f);
    test_run("SSH Settings and refresh supersede blocked reads", refresh_race);
    test_run("SSH auth and project resets cancel stale reads and destruction releases ownership", reset_and_destroy);
    test_run("SSH global and project views reuse sessions and preserve cycling and reconnect", sessions_shared);
    test_run("SSH native search handles Escape, Tab, refresh and auth scope reset", search_control_keys_and_reset);
    test_run("SSH global navigation uses admin capability gating", navigation_gated);
}
