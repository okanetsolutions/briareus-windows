#include "screen_fixture.h"
#include "screen_conversation_internal.h"
#include "saved_prompts_internal.h"
#include "modal_driver.h"
#include "resource.h"
#include "suites.h"

static Route routes[] = {{"GET", "/prompts", "read"}, {"POST", "/prompts", "admin"},
    {"PUT", "/prompts/{id}", "admin"}, {"DELETE", "/prompts/{id}", "admin"}, {"POST", "/sessions/{sessionId}/message", "manage"}};
static const char *library = "{\"prompts\":[{\"id\":41,\"title\":\"Scoped\",\"body\":\"saved\\nbody\",\"repo\":\"o/r\",\"sortOrder\":1},{\"id\":42,\"title\":\"Shared\",\"body\":\"global\",\"repo\":null,\"sortOrder\":2}]}";
static ConversationScreen *open_composer(ScreenFixture *f, const char *repo) {
    Session ss = {0}; ss.raw = json_object(); json_set_str(ss.raw, "id", "s1"); json_set_str(ss.raw, "repo", repo); json_set_str(ss.raw, "status", "idle");
    Screen *screen = conversation_screen_new(&ss); session_free(&ss);
    if (!f->pane) screen_fixture_pane(f, screen); else screen->pane = f->pane;
    screen->vt->visible(screen, true); return (ConversationScreen *)screen;
}
static void init(ScreenFixture *f) { screen_fixture_init(f, routes, sizeof routes / sizeof *routes); f->read_response = library; }
static void load(ScreenFixture *f, PromptPicker *p) { prompt_picker_read(p); screen_fixture_pump(f, 1); CHECK(p->loaded); }
static void clear(ScreenFixture *f) { json_free(g_store.prompt_state); g_store.prompt_state = NULL; screen_fixture_clear(f); }
static void expire(PromptPicker *p) { json_set_num((Json *)prompt_picker_state(p), "until", 0); }
static void composer_insert_and_gates(void) {
    ScreenFixture f; init(&f); g_store.device.permission = "read"; char *repos[] = {"o/r"}; g_store.device.repos = repos; g_store.device.repo_count = 1;
    ConversationScreen *s = open_composer(&f, "o/r"); PromptPicker *p = s->prompts;
    CHECK(prompt_picker_offered(p)); CHECK(screen_conversation_can_compose(s)); CHECK(!screen_conversation_can_message(s));
    load(&f, p); CHECK_INT(json_count(p->rows), 2);
    screen_conversation_set_composer_text(s, "left middle right"); SendMessageW(s->composer, EM_SETSEL, 5, 11);
    DWORD first = 0, last = 0; SendMessageW(s->composer, EM_GETSEL, (WPARAM)&first, (LPARAM)&last); CHECK_INT(first, 5); CHECK_INT(last, 11);
    prompt_picker_insert_row(p, json_at(p->rows, 0)); CHECK_OWNED_STR(screen_conversation_composer_text(s), "left middle right\n\nsaved\nbody");
    SendMessageW(s->composer, EM_GETSEL, (WPARAM)&first, (LPARAM)&last); CHECK_INT(first, 32); CHECK_INT(first, last);
    CHECK_INT(attach_list_count(s->files), 0); CHECK_INT(f.writes, 0);
    screen_conversation_send_message(s); CHECK_INT(f.writes, 0);
    screen_conversation_set_composer_text(s, " \t\n"); prompt_picker_insert_row(p, json_at(p->rows, 1)); CHECK_OWNED_STR(screen_conversation_composer_text(s), "global");
    Json *old = json_clone(json_at(p->rows, 0));
    prompt_picker_visible(p, false); prompt_picker_insert_row(p, old); CHECK_OWNED_STR(screen_conversation_composer_text(s), "global");
    prompt_picker_visible(p, true); load(&f, p);
    json_set_str(s->initial.raw, "repo", "o/other"); CHECK(!prompt_picker_offered(p)); prompt_picker_insert_row(p, old);
    CHECK_OWNED_STR(screen_conversation_composer_text(s), "global"); json_set_str(s->initial.raw, "repo", "o/r");
    json_set_str(s->initial.raw, "id", "s2"); prompt_picker_insert_row(p, old); CHECK_OWNED_STR(screen_conversation_composer_text(s), "global"); json_set_str(s->initial.raw, "id", "s1");
    g_store.device.permission = "manage"; CHECK(!prompt_picker_offered(p)); g_store.device.permission = "read";
    CHECK(!prompt_picker_writable(p, "create_prompt")); CHECK(!prompt_picker_writable(p, "update_prompt")); CHECK(!prompt_picker_writable(p, "delete_prompt"));
    g_store.route_count = 0; CHECK(!prompt_picker_offered(p)); g_store.route_count = sizeof routes / sizeof *routes;
    ApiClient *client = g_store.client; g_store.client = NULL; CHECK(!prompt_picker_offered(p)); g_store.client = client;
    json_free(old); s->base.vt->destroy(&s->base); clear(&f);
}
static PromptPicker *modal_picker;
static Json *modal_row;
static ConversationScreen *modal_owner;
static void modal_action(Screen *unused, int action, intptr_t arg, POINT pt) {
    (void)unused; (void)arg; (void)pt;
    if (action == 1) prompt_picker_edit(modal_picker, modal_row);
    else if (action == 2) prompt_picker_delete(modal_picker, modal_row);
    else prompt_picker_save(modal_picker);
}
static const ScreenVTable modal_vt = {.action = modal_action};
static BOOL CALLBACK fill_dialog(HWND hwnd, LPARAM arg) {
    (void)arg; if (!GetDlgItem(hwnd, IDC_EDIT_TITLE)) return TRUE;
    SetDlgItemTextW(hwnd, IDC_EDIT_TITLE, L"Changed"); SetDlgItemTextW(hwnd, IDC_EDIT_BODY, L"edited\r\nbody"); return FALSE;
}
static void fill(void) { EnumThreadWindows(GetCurrentThreadId(), fill_dialog, 0); }
static void hide_owner(void) { modal_owner->base.vt->visible(&modal_owner->base, false); }
static void destroy_owner(void) { modal_owner->base.vt->destroy(&modal_owner->base); modal_owner = NULL; }
static void change_auth(void) { g_store.device.permission = "manage"; }
static void run_modal(int action, int answer, void (*during)(void)) {
    Screen driver = {0}; driver.vt = &modal_vt;
    modal_driver_action_during(&driver, action, 0, answer, during);
}
static void dialogs_and_write_lifecycle(void) {
    ScreenFixture f; init(&f); ConversationScreen *s = open_composer(&f, "o/r"); PromptPicker *p = s->prompts; load(&f, p);
    modal_picker = p; modal_owner = s; modal_row = json_clone(json_at(p->rows, 0));
    screen_conversation_set_composer_text(s, "authored");
    run_modal(1, IDCANCEL, fill); CHECK_INT(f.writes, 0); CHECK_STR(json_str(json_get(saved_prompt_find(p->rows, 41), "title")), "Scoped");
    run_modal(2, IDNO, NULL); CHECK_INT(f.writes, 0); CHECK_INT(json_count(p->rows), 2);
    f.write_response = "{\"prompt\":{\"id\":41,\"title\":\"Changed\",\"body\":\"edited\\nbody\",\"repo\":\"o/r\"}}";
    run_modal(1, IDOK, fill); screen_fixture_pump(&f, 2); CHECK_INT(f.writes, 1);
    CHECK_STR(f.method, "PUT"); CHECK_STR(f.url, "https://example.com/api/v1/prompts/41");
    CHECK_INT(json_count(f.body), 2); CHECK_STR(json_str(json_get(f.body, "title")), "Changed"); CHECK_STR(json_str(json_get(f.body, "body")), "edited\nbody");
    CHECK(json_is_null(json_get(f.body, "repo"))); CHECK_OWNED_STR(screen_conversation_composer_text(s), "authored");
    // A failed edit keeps both dialog values for explicit Resume, without touching the composer.
    f.write_status = 400; f.write_response = "{\"error\":\"fixture rejection\"}";
    run_modal(1, IDOK, fill); screen_fixture_pump(&f, 1); CHECK_INT(f.writes, 2); CHECK_STR(p->title, "Changed"); CHECK_STR(p->body, "edited\nbody"); CHECK(p->notice != NULL);
    CHECK(!prompt_picker_writable(p, "update_prompt")); expire(p); load(&f, p);
    // Auth changes inside a live dialog cannot apply its accepted values or send.
    run_modal(1, IDOK, change_auth); CHECK_INT(f.writes, 2); g_store.device.permission = "admin";
    run_modal(1, IDOK, hide_owner); CHECK_INT(f.writes, 2); s->base.vt->visible(&s->base, true); load(&f, p);
    // Retiring the screen from the nested modal loop is safe; no write uses the freed owner.
    run_modal(1, IDOK, destroy_owner); CHECK_INT(f.writes, 2);
    json_free(modal_row); modal_row = NULL; clear(&f);
}
static void failures_cooldowns_and_uncertainty(void) {
    ScreenFixture f; init(&f); ConversationScreen *s = open_composer(&f, "o/r"); PromptPicker *p = s->prompts;
    f.read_status = 404; load(&f, p); CHECK(p->error != NULL); CHECK(p->rows == NULL);
    prompt_picker_read(p); CHECK(p->read == NULL); CHECK_INT(f.reads, 1);
    expire(p); f.read_status = 200; f.read_response = "{}"; load(&f, p); CHECK(p->error != NULL);
    f.read_response = "{\"prompts\":[]}"; load(&f, p); CHECK(p->error == NULL); CHECK_INT(json_count(p->rows), 0);
    f.read_response = library; load(&f, p);
    f.write_status = 429; f.write_retry = "120"; f.write_response = "{\"error\":\"limited\"}";
    prompt_picker_write(p, "create_prompt", saved_prompt_create("New", "body", "o/r")); screen_fixture_pump(&f, 1);
    double until = json_num_or(json_get(prompt_picker_state(p), "until"), 0); CHECK(until >= (double)GetTickCount64() / 1000 + 119);
    CHECK(!prompt_picker_writable(p, "create_prompt")); expire(p);
    f.fail_write = true; prompt_picker_write(p, "create_prompt", saved_prompt_create("New", "body", NULL)); screen_fixture_pump(&f, 1);
    CHECK(json_bool_is(json_get(prompt_picker_state(p), "uncertain"), true)); CHECK_INT(f.writes, 2);
    expire(p); load(&f, p); CHECK(!prompt_picker_writable(p, "create_prompt"));
    prompt_picker_write(p, "create_prompt", saved_prompt_create("New", "body", NULL)); CHECK_INT(f.writes, 2);
    s->base.vt->destroy(&s->base); s = open_composer(&f, "o/r"); p = s->prompts; CHECK(!prompt_picker_writable(p, "delete_prompt"));
    s->base.vt->destroy(&s->base); clear(&f);
}
// Freeze only the first read. Its response is captured before the write and is
// released after the post-delete read, proving the stale result cannot resurrect it.
typedef struct { ScreenFixture f; LONG calls; HANDLE old_started, old_release; } Ordered;
static bool ordered_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body, size_t bytes,
    int timeout, int *status, char **type, char **retry, char **response, size_t *len, char **error) {
    Ordered *o = ctx;
    if (str_eq(method, "GET") && InterlockedIncrement(&o->calls) == 1) {
        SetEvent(o->old_started); WaitForSingleObject(o->old_release, 10000);
        *status = 200; *type = xstrdup("application/json"); *retry = NULL; *error = NULL; *response = xstrdup(library); *len = strlen(*response); return true;
    }
    return screen_fixture_transport(&o->f, method, url, headers, body, bytes, timeout, status, type, retry, response, len, error);
}
static void read_delete_order_and_teardown(void) {
    Ordered o = {0}; init(&o.f); o.old_started = CreateEventW(NULL, TRUE, FALSE, NULL); o.old_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    api_client_set_transport(o.f.client, ordered_transport, &o);
    ConversationScreen *s = open_composer(&o.f, "o/r"); PromptPicker *p = s->prompts;
    prompt_picker_read(p); CHECK_INT(WaitForSingleObject(o.old_started, 10000), WAIT_OBJECT_0);
    o.f.write_response = "{\"ok\":true}"; o.f.read_response = "{\"prompts\":[]}";
    Json *args = json_object(); json_set_num(args, "id", 41); prompt_picker_write(p, "delete_prompt", args);
    screen_fixture_pump(&o.f, 2); CHECK_INT(json_count(p->rows), 0); CHECK_STR(o.f.method, "DELETE");
    SetEvent(o.old_release); screen_fixture_pump(&o.f, 1); CHECK_INT(json_count(p->rows), 0); CHECK(p->loaded);
    // Successful creation emits chosen project/global scopes; success invalidates and refreshes exactly once.
    o.f.write_response = "{\"prompt\":{\"id\":43,\"title\":\"New\",\"body\":\"body\",\"repo\":null}}";
    prompt_picker_write(p, "create_prompt", saved_prompt_create("New", "body", NULL)); screen_fixture_pump(&o.f, 2);
    CHECK_STR(o.f.method, "POST"); CHECK(json_is_null(json_get(o.f.body, "repo")));
    prompt_picker_write(p, "create_prompt", saved_prompt_create("New", "body", "o/r")); screen_fixture_pump(&o.f, 2); CHECK_STR(json_str(json_get(o.f.body, "repo")), "o/r");
    o.f.block_write = true; ResetEvent(o.f.entered); ResetEvent(o.f.resume);
    prompt_picker_write(p, "create_prompt", saved_prompt_create("Pending", "body", NULL)); CHECK_INT(WaitForSingleObject(o.f.entered, 10000), WAIT_OBJECT_0);
    s->base.vt->destroy(&s->base); s = open_composer(&o.f, "o/r"); p = s->prompts;
    CHECK(!prompt_picker_writable(p, "create_prompt")); SetEvent(o.f.resume); screen_fixture_pump(&o.f, 1); CHECK(prompt_picker_writable(p, "create_prompt"));
    s->base.vt->destroy(&s->base); CloseHandle(o.old_started); CloseHandle(o.old_release); clear(&o.f);
}
static void hidden_create_retires_form_and_abandoned_reads(void) {
    ScreenFixture f; init(&f); ConversationScreen *s = open_composer(&f, NULL); PromptPicker *p = s->prompts;
    modal_picker = p; modal_owner = s;
    screen_conversation_set_composer_text(s, "authored");
    // The initial read is in flight when Save opens; it is not the owner of this form.
    f.block_read = true; prompt_picker_read(p); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    f.block_write = true; ResetEvent(f.entered);
    f.write_response = "{\"prompt\":{\"id\":51,\"title\":\"Changed\",\"body\":\"edited\\nbody\",\"repo\":null}}";
    run_modal(3, IDOK, fill); CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0);
    CHECK(p->read == NULL); CHECK_INT(p->editing, 0);
    s->base.vt->visible(&s->base, false); SetEvent(f.resume); screen_fixture_pump(&f, 2);
    CHECK_INT(p->editing, -1); CHECK(p->title == NULL); CHECK_INT(f.writes, 1);
    CHECK_OWNED_STR(screen_conversation_composer_text(s), "authored");
    s->base.vt->visible(&s->base, true); f.block_read = false; load(&f, p); CHECK_INT(f.writes, 1);
    // A new read's delayed completion cannot land on a changed auth snapshot.
    ResetEvent(f.entered); ResetEvent(f.resume); f.block_read = true; prompt_picker_read(p);
    CHECK_INT(WaitForSingleObject(f.entered, 10000), WAIT_OBJECT_0); g_store.device.permission = "manage";
    SetEvent(f.resume); screen_fixture_pump(&f, 1); CHECK(!p->loaded); CHECK(p->rows == NULL);
    g_store.device.permission = "admin"; s->base.vt->destroy(&s->base); clear(&f);
}
void app_saved_prompt_tests(void) {
    screen_fixture_prepare();
    test_run("composer saved prompt insertion stays local and bound to current owner and permission", composer_insert_and_gates);
    test_run("saved prompt dialogs cancel preserve scope retain failed edits and survive reentrant retirement", dialogs_and_write_lifecycle);
    test_run("saved prompt read failures cooldowns and uncertain creation survive navigation", failures_cooldowns_and_uncertainty);
    test_run("saved prompt cancelled old read cannot resurrect deletion and pending writes survive teardown", read_delete_order_and_teardown);
    test_run("hidden successful prompt creation cannot reappear as Resume and stale auth reads are ignored", hidden_create_retires_form_and_abandoned_reads);
}
