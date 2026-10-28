#include "screen_fixture.h"
#include "screen_settings_internal.h"
#include "forge_site_internal.h"
#include "modal_driver.h"
#include "suites.h"

static Pane *leaving_pane;
static HWND leaving_edit;
static bool leave_result, destroy_pane;

static void check_leave(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)action; (void)arg; (void)pt;
    leave_result = pane_can_leave(leaving_pane);
}
static void retire_owner(void) {
    // Connection rebuilding clears the stack; detached-window teardown also destroys the pane.
    if (destroy_pane) pane_destroy(leaving_pane);
    else pane_set_root(leaving_pane, NULL);
    if (!destroy_pane) CHECK(pane_top(leaving_pane) == NULL);
    // With a surviving pane, the owner and its controls live until confirmation returns.
    if (!destroy_pane) CHECK(IsWindow(leaving_edit));
}
static Screen *new_form(int kind) {
    switch (kind) {
    case 0: return meeting_settings_screen_new();
    case 1: return project_settings_screen_new(NULL, NULL);
    case 2: return provider_settings_screen_new(NULL, NULL);
    case 3: return screen_settings_ssh_settings_screen_new(NULL, NULL);
    case 4: return screen_settings_forge_settings_screen_new(NULL, NULL);
    default: return db_server_settings_screen_new(NULL, NULL);
    }
}
static void settings_leave_lifetime(void) {
    static const ScreenVTable vt = {.action = check_leave};
    Screen navigation = {.vt = &vt};
    const int answers[] = {IDCANCEL, IDOK};
    for (int kind = 0; kind < 6; kind++) {
        for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) {
            ScreenFixture f; screen_fixture_init(&f, NULL, 0);
            Screen *s = new_form(kind); screen_fixture_pane(&f, s); pane_push(f.pane, s);
            leaving_pane = f.pane;
            leaving_edit = FindWindowExW(pane_hwnd(f.pane), NULL, L"EDIT", NULL);
            CHECK(leaving_edit != NULL);
            SetWindowTextW(leaving_edit, L"unsaved fixture draft");
            s->vt->command(s, GetDlgCtrlID(leaving_edit), EN_CHANGE, leaving_edit);
            modal_driver_action(&navigation, 0, 0, IDCANCEL);
            CHECK(!leave_result); CHECK(pane_top(f.pane) == s); CHECK(IsWindow(leaving_edit));
            // A second prompt proves Cancel retained the dirty state, while Discard clears it.
            modal_driver_action(&navigation, 0, 0, IDOK);
            CHECK(leave_result); CHECK(pane_can_leave(f.pane));
            SetWindowTextW(leaving_edit, L"another unsaved fixture draft");
            s->vt->command(s, GetDlgCtrlID(leaving_edit), EN_CHANGE, leaving_edit);
            destroy_pane = i != 0;
            modal_driver_action_during(&navigation, 0, 0, answers[i], retire_owner);
            CHECK(!leave_result); CHECK(!IsWindow(leaving_edit));
            if (destroy_pane) f.pane = NULL;
            leaving_pane = NULL; leaving_edit = NULL;
            screen_fixture_clear(&f);
        }
    }
}

static ForgeSite *leaving_site;
static bool site_can_leave(Screen *s) { (void)s; return forge_site_can_leave(leaving_site); }
static void site_destroy(Screen *s) { forge_site_free(leaving_site); screen_release(s); }
static void site_layout(Screen *s, Doc *doc) { (void)s; (void)doc; }
static void site_header(Screen *s, HeaderInfo *info) { (void)s; (void)info; }
static void forge_site_leave_lifetime(void) {
    static const ScreenVTable vt = {.action = check_leave};
    static const ScreenVTable host_vt = {.destroy = site_destroy, .can_leave = site_can_leave, .layout = site_layout, .header = site_header};
    Screen navigation = {.vt = &vt};
    const int answers[] = {IDCANCEL, IDOK};
    for (size_t i = 0; i < sizeof answers / sizeof *answers; i++) {
        ScreenFixture f; screen_fixture_init(&f, NULL, 0);
        Screen *host = xcalloc(1, sizeof *host); host->vt = &host_vt;
        screen_fixture_pane(&f, host); pane_push(f.pane, host);
        Json *row = json_parsez("{\"id\":1}");
        leaving_site = forge_site_new(host, 3000, 1, row, row); json_free(row);
        leaving_pane = f.pane; leaving_edit = leaving_site->edits[ED_SCRIPT];
        CHECK(leaving_edit != NULL); leaving_site->changed[ED_SCRIPT] = true;
        modal_driver_action(&navigation, 0, 0, IDCANCEL);
        CHECK(!leave_result); CHECK(leaving_site->changed[ED_SCRIPT]);
        modal_driver_action(&navigation, 0, 0, IDOK);
        CHECK(leave_result); CHECK(!leaving_site->changed[ED_SCRIPT]);
        leaving_site->changed[ED_ENV] = true;
        destroy_pane = i != 0;
        modal_driver_action_during(&navigation, 0, 0, answers[i], retire_owner);
        CHECK(!leave_result); CHECK(!IsWindow(leaving_edit));
        if (destroy_pane) f.pane = NULL;
        leaving_site = NULL; leaving_pane = NULL; leaving_edit = NULL;
        screen_fixture_clear(&f);
    }
}
void app_leave_tests(void) {
    screen_fixture_prepare();
    test_run("settings leave confirmations survive stack and pane teardown", settings_leave_lifetime);
    test_run("Forge site leave confirmation survives host teardown", forge_site_leave_lifetime);
}
