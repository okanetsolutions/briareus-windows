#include "screen_fixture.h"
#include "screen_settings_github_internal.h"
#include "modal_driver.h"
#include "suites.h"

static Route ROUTES[] = {
    {"GET", "/settings/github/accounts", "admin"}, {"POST", "/settings/github/accounts", "admin"},
    {"PUT", "/settings/github/accounts/{id}", "admin"}, {"DELETE", "/settings/github/accounts/{id}", "admin"},
};
static const char *ACCOUNT = "{\"id\":7,\"label\":\"Production\",\"login\":\"fixture-bot\",\"githubId\":42,\"scopes\":null,\"hasToken\":true,\"repos\":[\"o/r\"]}";

typedef struct { ScreenFixture fixture; GithubForm *form; } Form;
static void setup(Form *f, bool create) {
    memset(f, 0, sizeof *f);
    screen_fixture_init(&f->fixture, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Json *row = create ? NULL : json_parsez(ACCOUNT);
    f->form = (GithubForm *)screen_settings_github_settings_screen_new(row, NULL); json_free(row);
    screen_fixture_pane(&f->fixture, &f->form->base);
    f->form->base.vt->visible(&f->form->base, true);
}
static void cleanup(Form *f) {
    f->form->base.vt->destroy(&f->form->base); screen_fixture_clear(&f->fixture);
}
static void edit(Form *f, int field, const char *value) {
    screen_settings_set_edit_text(f->form->edits[field], value);
    f->form->base.vt->command(&f->form->base, ID_GITHUB_FIELD + field, EN_CHANGE, f->form->edits[field]);
}
static void save(Form *f) { screen_settings_github_save(f->form); screen_fixture_pump(&f->fixture, 1); }
static char *saved_response(void) { return xstrfmt("{\"account\":%s}", ACCOUNT); }
static void check_no_metadata(const Json *body) {
    CHECK(json_is_null(json_get(body, "id"))); CHECK(json_is_null(json_get(body, "login")));
    CHECK(json_is_null(json_get(body, "githubId"))); CHECK(json_is_null(json_get(body, "scopes")));
    CHECK(json_is_null(json_get(body, "hasToken")));
}

static void create_update_and_rotate(void) {
    Form f; setup(&f, true);
    CHECK(GetWindowLongPtrW(f.form->edits[GH_TOKEN], GWL_STYLE) & ES_PASSWORD);
    screen_settings_github_save(f.form); CHECK(f.form->req_save == NULL); CHECK(f.form->error != NULL);
    edit(&f, GH_LABEL, "Production"); edit(&f, GH_TOKEN, "fixture-secret");
    SettingsScreen settings = {0}; settings.projects = json_parsez("{\"list\":[{\"repo\":\"o/r\"},{\"repo\":\"o/second\"}]}");
    screen_settings_g_settings = &settings;
    screen_settings_github_toggle_repo(f.form, 0);
    CHECK_INT(json_count(f.form->repos), 1); CHECK(screen_settings_github_form_changed(f.form));
    // Refreshing the sidebar is tested separately, keep this lifecycle's request count deterministic.
    screen_settings_g_settings = NULL; json_free(settings.projects);
    char *response = saved_response(); f.fixture.write_response = response;
    save(&f);
    CHECK_STR(f.fixture.method, "POST"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/github/accounts");
    CHECK_INT(json_count(f.fixture.body), 3); check_no_metadata(f.fixture.body);
    CHECK_STR(json_str(json_get(f.fixture.body, "token")), "fixture-secret");
    CHECK_STR(json_str(json_at(json_get(f.fixture.body, "repos"), 0)), "o/r");
    CHECK_INT((int)f.form->id, 7); CHECK(!f.form->dirty);
    CHECK_STR(f.form->base.id, "settings-github:7"); CHECK(screen_settings_is_form_id(f.form->base.id));
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_TOKEN]), "");
    CHECK(json_is_null(json_get(f.form->row, "token")));
    edit(&f, GH_LABEL, "Renamed"); edit(&f, GH_TOKEN, "   ");
    screen_settings_github_toggle_repo(f.form, 0); // Unassign o/r using the same picker.
    save(&f);
    CHECK_STR(f.fixture.method, "PUT"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/github/accounts/7");
    CHECK_STR(json_str(json_get(f.fixture.body, "label")), "Renamed");
    CHECK_INT(json_count(f.fixture.body), 2); CHECK(json_is_null(json_get(f.fixture.body, "token"))); check_no_metadata(f.fixture.body);
    CHECK_INT(json_count(json_get(f.fixture.body, "repos")), 0);
    edit(&f, GH_TOKEN, "rotated-fixture"); save(&f);
    CHECK_STR(json_str(json_get(f.fixture.body, "token")), "rotated-fixture");
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_TOKEN]), "");
    CHECK_STR(json_str(json_get(f.form->row, "login")), "fixture-bot");
    free(response); cleanup(&f);
}

static void errors_preserve_drafts_without_retry(void) {
    const int statuses[] = {400, 404, 409, 500, 502, 503};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        Form f; setup(&f, false);
        edit(&f, GH_LABEL, "Unsaved label"); edit(&f, GH_TOKEN, "unsaved-fixture");
        screen_settings_github_toggle_repo(f.form, 0);
        f.fixture.write_status = statuses[i]; f.fixture.write_response = "{\"error\":\"Server explains assignment or verification failure\"}";
        f.fixture.write_retry = "1";
        save(&f);
        CHECK(strstr(f.form->error, "Server explains assignment or verification failure") != NULL);
        CHECK_INT(strstr(f.form->error, "It may still have been saved") != NULL, statuses[i] == 500);
        CHECK(f.form->dirty); CHECK(f.form->req_save == NULL); CHECK_INT(f.fixture.writes, 1);
        CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_LABEL]), "Unsaved label");
        CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_TOKEN]), "unsaved-fixture");
        CHECK_INT(json_count(f.form->repos), 0);
        CHECK_STR(json_str(json_get(f.form->row, "label")), "Production");
        // Pump beyond Retry-After so any scheduled write retry would reach the transport.
        Doc doc; doc_init(&doc); screen_fixture_draw(&f.form->base, &doc); doc_free(&doc);
        ULONGLONG end = GetTickCount64() + 1200;
        while (GetTickCount64() < end) {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_APP_REQUEST_DONE) store_handle_message(msg.message, msg.wParam, msg.lParam);
                else DispatchMessageW(&msg);
            }
            Sleep(1);
        }
        CHECK_INT(f.fixture.writes, 1); cleanup(&f);
    }
    Form f; setup(&f, false); edit(&f, GH_LABEL, "Unknown outcome"); f.fixture.fail_write = true;
    save(&f); CHECK(f.form->error != NULL); CHECK(f.form->dirty); CHECK_INT(f.fixture.writes, 1);
    CHECK(strstr(f.form->error, "It may still have been saved") != NULL);
    cleanup(&f);
}

static void verification_failures_precede_create(void) {
    const int statuses[] = {502, 503};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        Form f; setup(&f, true); edit(&f, GH_TOKEN, "unsaved-fixture");
        f.fixture.write_status = statuses[i]; f.fixture.write_response = "{\"error\":\"Verification refused\"}";
        save(&f);
        CHECK(strstr(f.form->error, "Verification refused") != NULL);
        CHECK(strstr(f.form->error, "It may still have been saved") == NULL);
        CHECK(f.form->dirty); CHECK_INT(f.fixture.writes, 1); cleanup(&f);
    }
}

static void delete_confirmation_and_errors(void) {
    Form f; setup(&f, false);
    pane_push(f.fixture.pane, &f.form->base);
    modal_driver_action(&f.form->base, ACT_GITHUB_DELETE, 0, IDCANCEL);
    CHECK(f.form->req_delete == NULL); CHECK_INT(f.fixture.writes, 0);
    edit(&f, GH_LABEL, "Retain on failed delete");
    f.fixture.write_status = 404; f.fixture.write_response = "{\"error\":\"Account no longer exists\"}";
    modal_driver_action(&f.form->base, ACT_GITHUB_DELETE, 0, IDOK); screen_fixture_pump(&f.fixture, 1);
    CHECK_STR(f.fixture.method, "DELETE"); CHECK_STR(f.fixture.url, "https://example.com/api/v1/settings/github/accounts/7");
    CHECK(f.fixture.body == NULL); CHECK(strstr(f.form->error, "Account no longer exists") != NULL);
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_LABEL]), "Retain on failed delete");
    f.fixture.write_status = 200; f.fixture.write_response = "{\"ok\":true}";
    modal_driver_action(&f.form->base, ACT_GITHUB_DELETE, 0, IDOK); screen_fixture_pump(&f.fixture, 1);
    CHECK_INT(f.fixture.writes, 2);
    CHECK(pane_root(f.fixture.pane) == NULL);
    f.form = NULL; screen_fixture_clear(&f.fixture);
}

static void section_capability_and_permissions(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base); Doc doc; doc_init(&doc);
    f.read_response = "{\"accounts\":[]}";
    base->vt->visible(base, true); screen_fixture_pump(&f, 1);
    CHECK(s->github_loaded); CHECK(screen_settings_github_offered());
    doc_begin(&doc, NULL, 500); screen_settings_github_section(s, &doc, 500, NULL); doc_end(&doc);
    bool new_offered = false;
    for (size_t i = 0; i < doc.count; i++) new_offered |= doc.items[i].action == ACT_NEW_GITHUB;
    CHECK(new_offered);
    const char *permissions[] = {"read", "manage", "admin"};
    for (size_t i = 0; i < sizeof permissions / sizeof *permissions; i++) {
        g_store.device.permission = (char *)permissions[i];
        g_store.route_count = i == 2 ? 0 : sizeof ROUTES / sizeof *ROUTES;
        CHECK(!screen_settings_github_offered()); base->vt->visible(base, true); CHECK(s->req_github == NULL);
        doc_begin(&doc, NULL, 500); screen_settings_github_section(s, &doc, 500, NULL); doc_end(&doc);
        CHECK_INT(doc.count, 0);
    }
    CHECK_INT(f.reads, 1); doc_free(&doc); base->vt->destroy(base); screen_fixture_clear(&f);
}

static void connection_change_and_cancellation(void) {
    for (int destroy = 0; destroy < 2; destroy++) {
        Form f; setup(&f, false); edit(&f, GH_LABEL, "Draft");
        f.fixture.block_write = true; char *response = saved_response(); f.fixture.write_response = response;
        screen_settings_github_save(f.form);
        CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        if (destroy) { f.form->base.vt->destroy(&f.form->base); f.form = NULL; }
        else g_store.device.id = "another-device";
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        if (!destroy) {
            CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_LABEL]), "Draft");
            screen_settings_github_save(f.form); CHECK(f.form->req_save == NULL); CHECK_INT(f.fixture.writes, 1);
            f.form->base.vt->destroy(&f.form->base);
        }
        free(response); screen_fixture_clear(&f.fixture);
    }
}

static void shared_forge_picker_preserves_projects(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base);
    json_object_set(s->projects, "list", json_parsez("[{\"repo\":\"o/r\"},{\"repo\":\"o/second\"},{\"repo\":\"o/r\"}]"));
    Json *row = json_parsez("{\"id\":8,\"organization\":\"fixture\",\"hasToken\":true,\"repos\":[\"o/gone\",\"o/r\"]}");
    ForgeForm *forge = (ForgeForm *)screen_settings_forge_settings_screen_new(row, NULL); json_free(row);
    forge->base.pane = f.pane;
    Json *choices = screen_settings_repos_choices(forge->repos);
    CHECK_INT(json_count(choices), 3); CHECK_STR(json_str(json_at(choices, 0)), "o/r");
    CHECK_STR(json_str(json_at(choices, 1)), "o/second"); CHECK_STR(json_str(json_at(choices, 2)), "o/gone");
    json_free(choices);
    screen_settings_forge_toggle_repo(forge, 1);
    CHECK_INT(json_count(forge->repos), 3); CHECK(forge->dirty);
    screen_settings_forge_toggle_repo(forge, 2);
    Json *expected = json_parsez("[\"o/second\",\"o/r\"]");
    CHECK(screen_settings_repos_same(forge->repos, expected)); json_free(expected);
    screen_settings_forge_toggle_repo(forge, 50); CHECK_INT(json_count(forge->repos), 2);
    forge->base.vt->destroy(&forge->base); base->vt->destroy(base); screen_fixture_clear(&f);
}

static void list_errors_and_cancelled_read(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
    screen_fixture_pane(&f, base);
    char *response = xstrfmt("{\"accounts\":[%s]}", ACCOUNT); f.read_response = response;
    screen_settings_github_load(s); screen_fixture_pump(&f, 1);
    CHECK_INT(json_count(json_get(s->github, "accounts")), 1);
    f.read_status = 503; f.read_response = "{\"error\":\"Registry unavailable\"}";
    screen_settings_github_load(s); screen_fixture_pump(&f, 1);
    CHECK(strstr(s->github_error, "Registry unavailable") != NULL); CHECK(s->req_github == NULL);
    f.block_read = true; screen_settings_github_load(s);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    base->vt->destroy(base); SetEvent(f.resume); screen_fixture_pump(&f, 1);
    free(response); screen_fixture_clear(&f);
}

static void automatic_labels_survive_rotation(void) {
    Form f; setup(&f, false);
    json_set_str(f.form->row, "label", "fixture-bot"); screen_settings_github_fill(f.form);
    edit(&f, GH_TOKEN, "rotated-fixture");
    f.fixture.write_response = "{\"account\":{\"id\":7,\"label\":\"next-bot\",\"login\":\"next-bot\",\"githubId\":84,\"hasToken\":true,\"scopes\":[],\"repos\":[\"o/r\"]}}";
    save(&f);
    CHECK(json_is_null(json_get(f.fixture.body, "label")));
    CHECK_STR(json_str(json_get(f.fixture.body, "token")), "rotated-fixture");
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_LABEL]), "next-bot");
    edit(&f, GH_LABEL, ""); save(&f);
    CHECK_STR(json_str(json_get(f.fixture.body, "label")), "");
    CHECK(json_is_null(json_get(f.fixture.body, "token"))); cleanup(&f);
}

static void pending_writes_freeze_edits(void) {
    const int statuses[] = {200, 409};
    for (size_t k = 0; k < sizeof statuses / sizeof *statuses; k++) {
        Form f; setup(&f, false); edit(&f, GH_LABEL, "Submitted label");
        f.fixture.block_write = true; f.fixture.write_status = statuses[k];
        char *response = saved_response();
        f.fixture.write_response = k ? "{\"error\":\"Assignment conflict\"}" : response;
        screen_settings_github_save(f.form);
        CHECK_INT(WaitForSingleObject(f.fixture.entered, 10000), WAIT_OBJECT_0);
        for (int field = 0; field < GH_COUNT; field++) CHECK(!IsWindowEnabled(f.form->edits[field]));
        screen_settings_github_toggle_repo(f.form, 0); CHECK_INT(json_count(f.form->repos), 1);
        screen_settings_github_save(f.form); CHECK_INT(f.fixture.writes, 1);
        Doc doc; doc_init(&doc); screen_fixture_draw(&f.form->base, &doc);
        for (size_t i = 0; i < doc.count; i++) CHECK(doc.items[i].action != ACT_GITHUB_REPO);
        doc_free(&doc);
        SetEvent(f.fixture.resume); screen_fixture_pump(&f.fixture, 1);
        for (int field = 0; field < GH_COUNT; field++) CHECK(IsWindowEnabled(f.form->edits[field]));
        edit(&f, GH_LABEL, "New draft after completion"); CHECK(f.form->dirty);
        free(response); cleanup(&f);
    }
}

static void project_changes_refresh_assignments(void) {
    const char *repos[] = {"[]", "[\"o/renamed\"]"};
    for (size_t k = 0; k < sizeof repos / sizeof *repos; k++) {
        ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
        Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base;
        screen_fixture_pane(&f, base);
        char *initial = xstrfmt("{\"accounts\":[%s]}", ACCOUNT); f.read_response = initial;
        screen_settings_github_load(s); screen_fixture_pump(&f, 1);
        CHECK_INT(json_count(json_get(json_at(json_get(s->github, "accounts"), 0), "repos")), 1);
        char *refreshed = xstrfmt("{\"accounts\":[{\"id\":7,\"repos\":%s}]}", repos[k]);
        f.read_response = refreshed; f.block_read = true;
        settings_projects_changed(0);
        if (s->req_github) {
            CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
            CHECK(!s->github_loaded); CHECK(s->github == NULL);
            Doc doc; doc_init(&doc); doc_begin(&doc, NULL, 500);
            screen_settings_github_section(s, &doc, 500, NULL); doc_end(&doc);
            for (size_t i = 0; i < doc.count; i++) CHECK(doc.items[i].action != ACT_OPEN_GITHUB);
            doc_free(&doc);
            f.read_status = 503; SetEvent(f.resume); screen_fixture_pump(&f, 1);
            CHECK(s->github_error != NULL); CHECK(s->github == NULL);
            f.block_read = false; f.read_status = 200;
            screen_settings_github_load(s); screen_fixture_pump(&f, 1);
        }
        CHECK_INT(f.reads, 3);
        const Json *row = json_at(json_get(s->github, "accounts"), 0);
        GithubForm *form = (GithubForm *)screen_settings_github_settings_screen_new(row, NULL);
        char *why = NULL; Json *body = screen_settings_github_body(form, &why);
        Json *expected = json_parsez(repos[k]);
        CHECK(json_equal(json_get(body, "repos"), expected));
        CHECK(why == NULL);
        json_free(expected); json_free(body); free(why); form->base.vt->destroy(&form->base);
        base->vt->destroy(base); screen_fixture_clear(&f); free(initial); free(refreshed);
    }
}

static Form *modal_form;
static bool modal_leave;
static void discard_action(Screen *base, int action, intptr_t arg, POINT pt) {
    (void)base; (void)action; (void)arg; (void)pt;
    modal_leave = modal_form->form->base.vt->can_leave(&modal_form->form->base);
}
static const ScreenVTable discard_driver_vt = {.action = discard_action};
static void destroy_during_modal(void) {
    GithubForm *form = modal_form->form;
    CHECK(!form->base.vt->can_leave(&form->base));
    screen_settings_github_save(form); CHECK(form->req_save == NULL);
    form->base.vt->destroy(&form->base); modal_form->form = NULL;
}
static void switch_client_during_modal(void) { g_store.client = NULL; }
static void switch_device_during_modal(void) { g_store.device.id = "another-device"; }

static void modal_teardown_and_connection_changes(void) {
    void (*const during[])(void) = {destroy_during_modal, switch_client_during_modal, switch_device_during_modal};
    for (int discard = 0; discard < 2; discard++) {
        for (size_t i = 0; i < sizeof during / sizeof *during; i++) {
            Form f; setup(&f, false); edit(&f, GH_LABEL, "Unsaved modal draft"); modal_form = &f;
            Screen driver = {.vt = &discard_driver_vt};
            Screen *target = discard ? &driver : &f.form->base;
            modal_driver_action_during(target, discard ? 0 : ACT_GITHUB_DELETE, 0, IDOK, during[i]);
            CHECK_INT(f.fixture.writes, 0);
            if (discard) CHECK(modal_leave);
            if (f.form) {
                CHECK(f.form->req_delete == NULL); CHECK(!screen_settings_github_current(f.form));
                screen_settings_github_save(f.form); CHECK(f.form->req_save == NULL);
                cleanup(&f);
            } else screen_fixture_clear(&f.fixture);
        }
    }
    modal_form = NULL;
}

static void discard_cancel_keeps_draft(void) {
    Form f; setup(&f, false); edit(&f, GH_TOKEN, "unsaved-fixture"); modal_form = &f;
    Screen driver = {.vt = &discard_driver_vt};
    modal_driver_action(&driver, 0, 0, IDCANCEL);
    CHECK(!modal_leave); CHECK(f.form->dirty);
    CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_TOKEN]), "unsaved-fixture");
    modal_driver_action(&driver, 0, 0, IDOK);
    CHECK(modal_leave); CHECK(!f.form->dirty); CHECK_INT(f.fixture.writes, 0);
    cleanup(&f); modal_form = NULL;
}

static bool retain_underlying_editor(Screen *base) { (void)base; return false; }
static void underlying_destroy(Screen *base) { screen_release(base); }
static const ScreenVTable underlying_vt = {.destroy = underlying_destroy, .can_leave = retain_underlying_editor};
static void delete_preserves_underlying_editor(void) {
    Form f; setup(&f, false);
    Screen *underlying = xcalloc(1, sizeof *underlying); underlying->vt = &underlying_vt;
    pane_push(f.fixture.pane, underlying); pane_push(f.fixture.pane, &f.form->base);
    f.fixture.write_response = "{\"ok\":true}";
    modal_driver_action(&f.form->base, ACT_GITHUB_DELETE, 0, IDOK); screen_fixture_pump(&f.fixture, 1);
    CHECK(pane_root(f.fixture.pane) == underlying); CHECK(pane_top(f.fixture.pane) == underlying);
    CHECK(!pane_can_leave(f.fixture.pane));
    screen_fixture_clear(&f.fixture);
}

static void shared_picker_matches_project_case(void) {
    ScreenFixture f; screen_fixture_init(&f, ROUTES, sizeof ROUTES / sizeof *ROUTES);
    Screen *base = settings_screen_new(); SettingsScreen *s = (SettingsScreen *)base; screen_fixture_pane(&f, base);
    json_object_set(s->projects, "list", json_parsez("[{\"repo\":\"owner/repo\"},{\"repo\":\"OWNER/REPO\"}]"));
    Json *row = json_parsez("{\"id\":8,\"repos\":[\"Owner/Repo\"]}");
    GithubForm *github = (GithubForm *)screen_settings_github_settings_screen_new(row, NULL);
    ForgeForm *forge = (ForgeForm *)screen_settings_forge_settings_screen_new(row, NULL); json_free(row);
    github->base.pane = forge->base.pane = f.pane;
    Json *expected = json_parsez("[\"owner/repo\"]");
    CHECK(screen_settings_repos_same(github->repos, expected)); CHECK(screen_settings_repos_same(forge->repos, expected));
    Json *choices = screen_settings_repos_choices(github->repos);
    CHECK(json_equal(choices, expected)); json_free(choices);
    screen_settings_github_toggle_repo(github, 0); screen_settings_forge_toggle_repo(forge, 0);
    CHECK_INT(json_count(github->repos), 0); CHECK_INT(json_count(forge->repos), 0);
    screen_settings_github_toggle_repo(github, 0); screen_settings_forge_toggle_repo(forge, 0);
    CHECK(json_equal(github->repos, expected)); CHECK(json_equal(forge->repos, expected));
    CHECK(!screen_settings_github_form_changed(github));
    json_free(expected); github->base.vt->destroy(&github->base); forge->base.vt->destroy(&forge->base);
    base->vt->destroy(base); screen_fixture_clear(&f);
}

void app_github_settings_tests(void) {
    screen_fixture_prepare();
    test_run("GitHub create verification failures are definite refusals", verification_failures_precede_create);
    test_run("GitHub project delete and rename reload account assignments", project_changes_refresh_assignments);
    test_run("GitHub delete preserves the underlying unsaved editor", delete_preserves_underlying_editor);
    test_run("GitHub modals tolerate destruction and connection changes", modal_teardown_and_connection_changes);
    test_run("GitHub discard cancellation preserves the draft", discard_cancel_keeps_draft);
    test_run("shared GitHub and Forge picker matches project names ignoring case", shared_picker_matches_project_case);
    test_run("GitHub account unchanged automatic labels follow token rotation", automatic_labels_survive_rotation);
    test_run("GitHub account pending writes disable edits and project actions", pending_writes_freeze_edits);
    test_run("shared Forge project picker retains removed projects and order independence", shared_forge_picker_preserves_projects);
    test_run("GitHub account list surfaces errors and cancels reads on teardown", list_errors_and_cancelled_read);
    test_run("GitHub account create update rotation and blank token omission", create_update_and_rotate);
    test_run("GitHub account server failures preserve edits and never retry writes", errors_preserve_drafts_without_retry);
    test_run("GitHub account delete confirms cancellation and preserves failed edits", delete_confirmation_and_errors);
    test_run("GitHub account section hidden for older servers and non-admin tokens", section_capability_and_permissions);
    test_run("GitHub account writes ignore changed connections and cancel on teardown", connection_change_and_cancellation);
}
