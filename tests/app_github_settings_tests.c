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
    const int statuses[] = {400, 404, 409, 502, 503};
    for (size_t i = 0; i < sizeof statuses / sizeof *statuses; i++) {
        Form f; setup(&f, false);
        edit(&f, GH_LABEL, "Unsaved label"); edit(&f, GH_TOKEN, "unsaved-fixture");
        screen_settings_github_toggle_repo(f.form, 0);
        f.fixture.write_status = statuses[i]; f.fixture.write_response = "{\"error\":\"Server explains assignment or verification failure\"}";
        f.fixture.write_retry = "1";
        save(&f);
        CHECK(strstr(f.form->error, "Server explains assignment or verification failure") != NULL);
        CHECK(f.form->dirty); CHECK(f.form->req_save == NULL); CHECK_INT(f.fixture.writes, 1);
        CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_LABEL]), "Unsaved label");
        CHECK_OWNED_STR(screen_settings_edit_text(f.form->edits[GH_TOKEN]), "unsaved-fixture");
        CHECK_INT(json_count(f.form->repos), 0);
        CHECK_STR(json_str(json_get(f.form->row, "label")), "Production");
        // A layout and timer delivery must not resubmit a failed write, including throttled verification.
        Doc doc; doc_init(&doc); screen_fixture_draw(&f.form->base, &doc); doc_free(&doc);
        if (f.form->base.vt->timer) f.form->base.vt->timer(&f.form->base, 1);
        CHECK_INT(f.fixture.writes, 1); cleanup(&f);
    }
    Form f; setup(&f, false); edit(&f, GH_LABEL, "Unknown outcome"); f.fixture.fail_write = true;
    save(&f); CHECK(f.form->error != NULL); CHECK(f.form->dirty); CHECK_INT(f.fixture.writes, 1);
    cleanup(&f);
}

static void delete_confirmation_and_errors(void) {
    Form f; setup(&f, false);
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
    CHECK_INT(f.fixture.writes, 2); CHECK(!f.form->dirty); cleanup(&f);
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

void app_github_settings_tests(void) {
    screen_fixture_prepare();
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
