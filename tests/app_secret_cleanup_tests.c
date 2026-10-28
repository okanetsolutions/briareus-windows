#include "screen_fixture.h"
#include "screen_settings_internal.h"
#include "suites.h"
#include "secret_heap_sources.h"

// Screen storage and UI strings are released by the real app, outside the fixture allocator.
#undef xcalloc
#undef xstrdup
#undef xstrfmt
#undef str_trim

static Screen *captured_clone;
static void capture_clone(Screen *screen) { captured_clone = screen; }
static void observed_scrub(Json *body) { heap_mark_sensitive(); heap_ssh_secrets_scrub(body); }
#undef ssh_secrets_scrub
#define ssh_secrets_scrub observed_scrub
#define screen_settings_ssh_settings_screen_new cleanup_ssh_new
#define screen_settings_ssh_save cleanup_ssh_save
#define app_show_detail capture_clone
Screen *cleanup_ssh_new(const Json *row, const Json *defaults);
void cleanup_ssh_save(SshForm *s);
#include "../app/screen_settings_ssh.c"
#undef screen_settings_ssh_settings_screen_new
#undef screen_settings_ssh_save
#undef app_show_detail
#include "secret_heap_unmap.h"

static void constructor_and_clone_erasure(void) {
    static Route routes[] = {{"GET", "/settings/ssh/servers", "admin"}, {"POST", "/settings/ssh/servers", "admin"}};
    ScreenFixture fixture; screen_fixture_init(&fixture, routes, sizeof routes / sizeof *routes);
    Json *row = json_parsez("{\"id\":7,\"repo\":\"o/r\",\"host\":\"fixture.invalid\",\"username\":\"fixture\",\"port\":22,\"hasPassword\":true,\"hasSudoPassword\":true}");
    SshForm *form = (SshForm *)cleanup_ssh_new(row, NULL);
    screen_fixture_pane(&fixture, &form->base); form->base.vt->visible(&form->base, true);
    for (int field = S_PASSWORD; field <= S_SUDO_PASSWORD; field++) {
        screen_settings_set_edit_text(form->edits[field], heap_marker);
        form->base.vt->command(&form->base, ID_SSH_FIELD + field, EN_CHANGE, form->edits[field]);
    }
    heap_reset_counts(); captured_clone = NULL;
    form->base.vt->action(&form->base, ACT_SSH_CLONE, 0, (POINT){0});
    CHECK(captured_clone != NULL);
    // The constructor's two duplicated JSON strings were observed with the sentinel, then fully
    // zeroed before json_object_remove freed them; retained clone drafts remain separately owned.
    CHECK(heap_erased >= 2); CHECK_INT(heap_unwiped, 0);
    if (captured_clone) {
        SshForm *clone = (SshForm *)captured_clone;
        CHECK_INT(clone->id, 0); CHECK(clone->dirty);
        for (int f = 0; f < SSH_SECRET_COUNT; f++) {
            CHECK(json_is_null(json_get(clone->row, SSH_SECRET_KEYS[f])));
            CHECK(!clone->secrets.fields[f].stored); CHECK(!clone->secrets.fields[f].remove);
            CHECK(str_eq(clone->secrets.fields[f].text, heap_marker));
        }
        heap_mark_sensitive(); captured_clone->vt->destroy(captured_clone); captured_clone = NULL;
    }
    heap_mark_sensitive(); form->base.vt->destroy(&form->base); json_free(row); screen_fixture_clear(&fixture);
    CHECK_INT(heap_unwiped, 0); CHECK_INT(heap_live, 0);
}
void app_secret_cleanup_tests(void) {
    screen_fixture_prepare();
    test_run("SSH actual clone constructor erases private JSON copies before free", constructor_and_clone_erasure);
}
