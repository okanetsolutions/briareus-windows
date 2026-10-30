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
    Request *old = p->req; ssh_server_folder_toggle(p->folded, "o/one", "");
    g_store.device.id = "another-device"; servers_ssh_scope_changed(); CHECK(old->cancelled);
    CHECK_INT(json_count(p->rows), 0); CHECK_INT(json_count(p->folded), 0);
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
    Doc layout; doc_init(&layout); global->loaded = true;
    json_array_push(global->rows, json_clone(row));
    RECT content = pane_content_rect(f.pane);
    doc_begin(&layout, NULL, pane_content_width(f.pane));
    project_ssh_layout(global, &layout, layout.width); doc_end(&layout);
    CHECK_INT(layout.region_count, 1);
    int folder_top = -1;
    for (size_t i = 0; i < layout.count; i++) if (layout.items[i].action == 700 + A_FOLDER) folder_top = layout.items[i].rc.top;
    CHECK_INT(folder_top, layout.regions[0].view.top);
    bool tab = false;
    for (size_t i = 0; i < layout.count; i++) if (layout.items[i].action == 700 + A_TAB) {
        CHECK_INT(layout.items[i].rc.top, folder_top);
        CHECK(layout.items[i].rc.bottom < global->term_rc.top); tab = true;
    }
    CHECK(tab); CHECK(global->term_rc.top > layout.regions[0].view.top);
    CHECK(global->term_rc.bottom <= content.bottom - content.top);
    project_ssh_place(global, &content, 0, true);
    RECT terminal; GetWindowRect(term_hwnd(two), &terminal);
    MapWindowPoints(NULL, pane_hwnd(f.pane), (POINT *)&terminal, 2);
    CHECK_INT(terminal.top, content.top + global->term_rc.top);
    CHECK(terminal.bottom <= content.bottom);
    doc_free(&layout);

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
static void clicks_return_the_keyboard(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    f.read_response = "{\"servers\":[{\"id\":1,\"repo\":\"o/one\",\"label\":\"Web\",\"username\":\"deploy\",\"host\":\"fake.example\"}]}";
    Screen *s = ssh_servers_screen_new(); screen_fixture_pane(&f, s); pane_set_root(f.pane, s);
    screen_fixture_pump(&f, 1);
    launches = 0; term_set_launcher(launch, NULL);
    HWND pane = pane_hwnd(f.pane); POINT pt = {0};
    // Each click first takes the keyboard to the pane, as the pane's mouse_down does; the repaint places the session.
    SetFocus(pane); s->vt->action(s, 700 + A_SERVER, 0, pt); UpdateWindow(pane);
    Term *t = term_active(); CHECK(t != NULL); CHECK_INT(launches, 1);
    CHECK(t && GetFocus() == term_hwnd(t));
    // The session's own tab and its server change nothing else, yet it takes the keyboard back.
    SetFocus(pane); s->vt->action(s, 700 + A_TAB, 0, pt); UpdateWindow(pane);
    CHECK(t && GetFocus() == term_hwnd(t));
    SetFocus(pane); s->vt->action(s, 700 + A_SERVER, 0, pt); UpdateWindow(pane);
    CHECK(t && GetFocus() == term_hwnd(t)); CHECK_INT(launches, 1);
    // Folding a project in the list leaves the keyboard with the list.
    SetFocus(pane); s->vt->action(s, 700 + A_FOLDER, 0, pt); UpdateWindow(pane);
    CHECK(GetFocus() == pane);
    servers_ssh_sessions_reset(); term_set_launcher(NULL, NULL); screen_fixture_clear(&f);
}
static void navigation_gated(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 400); screen_projects_sidebar_top(&doc, 400, "ssh-servers"); doc_end(&doc);
    for (size_t i = 0; i < doc.count; i++) CHECK(doc.items[i].action != ACT_SSH);
    FooterRects footer = {0}; RECT bounds = {0, 0, 268, 80};
    screen_projects_sidebar_footer_paint(NULL, &bounds, &footer, false);
    CHECK(!IsRectEmpty(&footer.ssh_rc));
    CHECK(footer.ssh_rc.right < footer.settings_rc.left);
    CHECK(footer.settings_rc.right < footer.signout_rc.left);
    doc_free(&doc); g_store.device.permission = "read";
    doc_init(&doc); doc_begin(&doc, NULL, 400); screen_projects_sidebar_top(&doc, 400, NULL); doc_end(&doc);
    CHECK_INT(screen_fixture_find(&doc, "❯ SSH servers"), -1);
    screen_projects_sidebar_footer_paint(NULL, &bounds, &footer, false); CHECK(IsRectEmpty(&footer.ssh_rc));
    doc_free(&doc); screen_fixture_clear(&f);
}
static void sidebar_scroll_survives_resize(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1);
    Screen host = {0}; screen_fixture_pane(&f, &host);
    ProjectSsh *p = project_ssh_new(NULL, &host, 700); p->loaded = true;
    for (int i = 0; i < 40; i++) {
        Json *row = json_object(); json_set_num(row, "id", i); json_set_str(row, "repo", "o/one");
        json_set_str(row, "label", "Synthetic server"); json_set_str(row, "host", "fake.example");
        json_array_push(p->rows, row);
    }
    Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 900); project_ssh_layout(p, &doc, 900); doc_end(&doc);
    CHECK_INT(doc.region_count, 1); CHECK(doc.regions[0].max > 0);
    CHECK(doc.regions[0].view.top > 0);
    CHECK(doc.regions[0].view.bottom <= pane_content_rect(f.pane).bottom - pane_content_rect(f.pane).top);
    CHECK(doc_region_wheel(&doc, 1, doc.regions[0].view.top + 1, 100000));
    int scroll = doc.regions[0].scroll; CHECK(scroll > 0);
    bool reached = false;
    for (size_t i = 0; i < doc.count; i++) {
        Item *it = &doc.items[i];
        if (it->action == 700 + A_SERVER && it->arg == 39) {
            CHECK(doc_hit(&doc, it->rc.left + 1, it->rc.top + 1) == (int)i); reached = true;
        }
    }
    CHECK(reached);
    doc_begin(&doc, NULL, 600); project_ssh_layout(p, &doc, 600); doc_end(&doc);
    CHECK_INT(doc.regions[0].scroll, scroll);
    doc_free(&doc); project_ssh_free(p); screen_fixture_clear(&f);
}
static void global_list_without_search(void) {
    ScreenFixture f; screen_fixture_init(&f, routes, 1); f.read_response = new_rows;
    Screen *s = ssh_servers_screen_new(); screen_fixture_pane(&f, s); pane_set_root(f.pane, s);
    screen_fixture_pump(&f, 1);
    Doc doc; doc_init(&doc); screen_fixture_draw(s, &doc);
    RECT content = pane_content_rect(f.pane); s->vt->place(s, &content, 0);
    CHECK(FindWindowExW(pane_hwnd(f.pane), NULL, L"EDIT", NULL) == NULL);
    s->vt->refresh(s); screen_fixture_pump(&f, 1); CHECK_INT(f.reads, 2);
    g_store.device.id = "replacement";
    screen_fixture_draw(s, &doc); screen_fixture_pump(&f, 1);
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
    test_run("SSH clicks on a session give it back the keyboard", clicks_return_the_keyboard);
    test_run("SSH sidebar reaches the last server and retains scroll on resize", sidebar_scroll_survives_resize);
    test_run("SSH global page refreshes and resets without a search control", global_list_without_search);
    test_run("SSH global navigation uses admin capability gating", navigation_gated);
}
