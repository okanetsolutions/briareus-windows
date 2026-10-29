// Real temporary files and fake upload transport; no server, token storage or large transport copies.
#include "screen_fixture.h"
#include "attach_list_internal.h"
#include "screen_conversation_internal.h"
#include "suites.h"

#include "attachment_fixture.h"

typedef struct { AttachFile *files; size_t count; char *error; } Selection;
static void selected(void *ctx, AttachFile *files, size_t count, char *error) {
    Selection *s = ctx; s->files = files; s->count = count; s->error = error;
}
static void check_selection_and_read_with_writer(DWORD extra_access) {
    UploadFixture f; init(&f); wchar_t path[MAX_PATH]; temp(path);
    HANDLE writer = CreateFileW(path, GENERIC_WRITE | extra_access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(writer != INVALID_HANDLE_VALUE); DWORD written = 0;
    CHECK(WriteFile(writer, "saved", 5, &written, NULL)); CHECK_INT(written, 5);
    CHECK(FlushFileBuffers(writer));
    Selection s = {0}; Attacher *a = attacher_new(selected, &s); const wchar_t *p = path;
    attacher_from_paths(a, &p, 1); pump(&f, WM_APP_ASYNC_DONE);
    CHECK(!attacher_pending(a)); CHECK(s.error == NULL); CHECK_INT(s.count, 1);
    if (s.count == 1) {
        CHECK_INT(s.files[0].len, 5); CHECK(s.files[0].bytes == NULL);
        volatile LONG stop = 0; char *error = attach_file_read(&s.files[0], &stop);
        CHECK(error == NULL); free(error); CHECK_INT(s.files[0].len, 5);
        CHECK(s.files[0].bytes != NULL);
        if (s.files[0].bytes) CHECK(memcmp(s.files[0].bytes, "saved", 5) == 0);
    }
    attach_files_free(s.files, s.count); free(s.error); attacher_free(a);
    CloseHandle(writer); DeleteFileW(path); screen_fixture_clear(&f.screen);
}
static void selection_and_read_allow_open_writer(void) {
    check_selection_and_read_with_writer(0);
    check_selection_and_read_with_writer(DELETE);
}
static void metadata_boundary_and_grouped_refusals(void) {
    UploadFixture f; init(&f); wchar_t paths[4][MAX_PATH];
    for (size_t i = 0; i < 4; i++) temp(paths[i]);
    resize_file(paths[0], 524288000); resize_file(paths[1], 524288001);
    DeleteFileW(paths[3]); CHECK(CreateDirectoryW(paths[3], NULL));
    Selection s = {0}; Attacher *a = attacher_new(selected, &s);
    const wchar_t *p[] = {paths[0], paths[1], paths[2], paths[3]};
    attacher_from_paths(a, p, 4); CHECK(attacher_pending(a)); pump(&f, WM_APP_ASYNC_DONE);
    CHECK(!attacher_pending(a)); CHECK_INT(s.count, 1); CHECK_INT(s.files[0].len, 524288000);
    CHECK(s.files[0].bytes == NULL); CHECK(s.error != NULL);
    size_t lines = s.error ? 1 : 0;
    for (const char *c = s.error; c && *c; c++) if (*c == '\n') lines++;
    CHECK_INT(lines, 3); CHECK_INT(f.calls, 0);
    attach_files_free(s.files, s.count); free(s.error); attacher_free(a);
    for (size_t i = 0; i < 3; i++) DeleteFileW(paths[i]);
    RemoveDirectoryW(paths[3]);
    screen_fixture_clear(&f.screen);
}
static void serialized_selections_read_current_bytes(void) {
    UploadFixture f; init(&f); f.block_first = true; Screen screen = {0};
    AttachList *l = attach_list_new(&screen, "message"); wchar_t paths[3][MAX_PATH];
    for (size_t i = 0; i < 3; i++) { temp(paths[i]); write_file(paths[i], "a"); }
    select_paths(l, paths, 2); CHECK(attach_list_uploading(l)); CHECK(attach_list_ids(l) == NULL);
    pump(&f, WM_APP_ASYNC_DONE); CHECK_INT(attach_list_count(l), 2);
    pump(&f, WM_APP_ASYNC_DONE); CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    // A second selection joins the same queue. The second file must still be unread.
    const wchar_t *last = paths[2]; attach_list_paths(l, &last, 1); pump(&f, WM_APP_ASYNC_DONE);
    CHECK_INT(f.calls, 1); CHECK_INT(attach_list_count(l), 3);
    write_file(paths[1], "changed"); attach_list_remove(l, 2);
    SetEvent(f.screen.resume); pump(&f, WM_APP_REQUEST_DONE); complete_file(&f);
    CHECK_INT(f.calls, 2); CHECK_INT(f.sizes[0], 1); CHECK_INT(f.sizes[1], 7); CHECK_INT(f.first[1], 'c');
    CHECK_INT(l->items[1]->size, 7); CHECK(!attach_list_uploading(l));
    Json *ids = attach_list_ids(l); CHECK_INT(json_count(ids), 2); CHECK_STR(json_str(json_at(ids, 1)), "file-2");
    attach_list_sent(l, ids); json_free(ids); CHECK_INT(attach_list_count(l), 0);
    attach_list_free(l); for (size_t i = 0; i < 3; i++) DeleteFileW(paths[i]);
    screen_fixture_clear(&f.screen);
}
static void removed_active_and_queued_chips_never_return(void) {
    UploadFixture f; init(&f); f.block_first = true; Screen screen = {0};
    AttachList *l = attach_list_new(&screen, "start_session"); wchar_t paths[3][MAX_PATH];
    for (size_t i = 0; i < 3; i++) { temp(paths[i]); write_file(paths[i], "x"); }
    select_paths(l, paths, 2); pump(&f, WM_APP_ASYNC_DONE); pump(&f, WM_APP_ASYNC_DONE);
    CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    attach_list_remove(l, 0); attach_list_remove(l, 0);
    const wchar_t *path = paths[2]; attach_list_paths(l, &path, 1); pump(&f, WM_APP_ASYNC_DONE);
    CHECK_INT(attach_list_count(l), 1); CHECK_INT(f.calls, 1); CHECK(attach_list_uploading(l));
    SetEvent(f.screen.resume); pump(&f, WM_APP_REQUEST_DONE); complete_file(&f);
    Json *ids = attach_list_ids(l); CHECK_INT(json_count(ids), 1); CHECK_STR(json_str(json_at(ids, 0)), "file-2"); json_free(ids);
    CHECK_INT(f.calls, 2); attach_list_free(l);
    for (size_t i = 0; i < 3; i++) DeleteFileW(paths[i]);
    screen_fixture_clear(&f.screen);
}
static void cancellation_at_each_worker_boundary(void) {
    for (int stage = 0; stage < 3; stage++) {
        UploadFixture f; init(&f); f.block_first = true; Screen screen = {0};
        AttachList *l = attach_list_new(&screen, "message"); wchar_t path[MAX_PATH]; temp(path); write_file(path, "x");
        const wchar_t *p = path; attach_list_paths(l, &p, 1);
        if (stage > 0) pump(&f, WM_APP_ASYNC_DONE);
        if (stage > 1) {
            pump(&f, WM_APP_ASYNC_DONE); CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
        }
        attach_list_free(l);
        if (stage < 2) pump(&f, WM_APP_ASYNC_DONE);
        else { SetEvent(f.screen.resume); pump(&f, WM_APP_REQUEST_DONE); }
        CHECK_INT(f.calls, stage == 2 ? 1 : 0);
        DeleteFileW(path); screen_fixture_clear(&f.screen);
    }
}
static void owner_scope_account_and_endpoint_changes(void) {
    for (int reason = 0; reason < 4; reason++) {
        UploadFixture f; init(&f); f.block_first = true; Screen screen = {0};
        AttachList *l = attach_list_new(&screen, "start_session"); attach_list_scope(l, "o/old");
        wchar_t paths[2][MAX_PATH]; for (size_t i = 0; i < 2; i++) { temp(paths[i]); write_file(paths[i], "x"); }
        select_paths(l, paths, 2); pump(&f, WM_APP_ASYNC_DONE); pump(&f, WM_APP_ASYNC_DONE);
        CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
        if (reason == 0) attach_list_scope(l, "o/new");
        if (reason == 1) { attach_lists_disconnect(); g_store.client = NULL; }
        if (reason == 2) g_store.client = NULL;
        if (reason == 3) g_store.device.id = "replaced-device";
        CHECK_INT(attach_list_count(l), 0); CHECK(attach_list_ids(l) == NULL);
        SetEvent(f.screen.resume); pump(&f, WM_APP_REQUEST_DONE);
        CHECK_INT(attach_list_count(l), 0); CHECK_INT(f.calls, 1);
        g_store.client = f.screen.client; attach_list_free(l);
        for (size_t i = 0; i < 2; i++) DeleteFileW(paths[i]);
        screen_fixture_clear(&f.screen);
    }
}
static void queued_read_revalidates_file_kind_and_size(void) {
    wchar_t path[MAX_PATH]; temp(path); write_file(path, "old");
    AttachFile file = {.name = xstrdup("file"), .path = _wcsdup(path), .len = 3}; volatile LONG stop = 0;
    write_file(path, "current"); CHECK(attach_file_read(&file, &stop) == NULL); CHECK_INT(file.len, 7); CHECK(memcmp(file.bytes, "current", 7) == 0);
    free(file.bytes); file.bytes = NULL;
    write_file(path, "s"); CHECK(attach_file_read(&file, &stop) == NULL); CHECK_INT(file.len, 1); free(file.bytes); file.bytes = NULL;
    resize_file(path, 524288001); char *error = attach_file_read(&file, &stop); CHECK(error != NULL); free(error); CHECK(file.bytes == NULL);
    resize_file(path, 0); error = attach_file_read(&file, &stop); CHECK(error != NULL); free(error);
    DeleteFileW(path); CHECK(CreateDirectoryW(path, NULL)); error = attach_file_read(&file, &stop); CHECK(error != NULL); free(error);
    RemoveDirectoryW(path); error = attach_file_read(&file, &stop); CHECK(error != NULL); free(error);
    InterlockedExchange(&stop, 1); CHECK(attach_file_read(&file, &stop) == NULL); CHECK(file.bytes == NULL); attach_file_clear(&file);
}
static void pasted_image_encoding_keeps_png_support(void) {
    AttachFile image = {.name = xstrdup("pasted.png"), .width = 1, .height = 1};
    image.pixels = xcalloc(4, 1); image.pixels[3] = 255; volatile LONG stop = 0;
    CHECK(SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED)));
    CHECK(attach_file_read(&image, &stop) == NULL);
    CHECK(image.len > 8); CHECK(image.pixels == NULL);
    const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10};
    CHECK(image.bytes != NULL); if (image.bytes) CHECK(memcmp(image.bytes, png, 8) == 0);
    attach_file_clear(&image); CoUninitialize();
}
static void conversation_gates_pending_selection(void) {
    UploadFixture f; init(&f); wchar_t path[MAX_PATH]; temp(path); write_file(path, "file"); const wchar_t *p = path;
    Session ss = {0}; ss.raw = json_parsez("{\"id\":\"s1\",\"repo\":\"o/r\",\"status\":\"idle\"}");
    ConversationScreen *c = (ConversationScreen *)conversation_screen_new(&ss); session_free(&ss);
    screen_fixture_pane(&f.screen, &c->base);
    c->composer = CreateWindowExW(0, L"EDIT", L"hello", WS_CHILD | ES_MULTILINE, 0, 0, 100, 20, f.screen.parent, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(c->composer != NULL); CHECK(screen_conversation_can_message(c));
    attach_list_paths(c->files, &p, 1); screen_conversation_send_message(c); CHECK(!c->busy);
    pump(&f, WM_APP_ASYNC_DONE); screen_conversation_send_message(c); CHECK(!c->busy); complete_file(&f);
    CHECK(!attach_list_uploading(c->files)); CHECK_INT(f.calls, 1);
    api_client_set_transport(f.screen.client, screen_fixture_transport, &f.screen);
    screen_conversation_send_message(c); CHECK(c->busy); CHECK(c->req_mutate != NULL);
    if (c->req_mutate) {
        CHECK_STR(json_str(json_at(json_get(c->req_mutate->args, "attachments"), 0)), "file-1");
        CHECK_STR(json_str(json_get(c->req_mutate->args, "sessionId")), "s1");
    }
    c->base.vt->destroy(&c->base); pump(&f, WM_APP_REQUEST_DONE); CHECK_INT(f.screen.writes, 1);
    DeleteFileW(path); screen_fixture_clear(&f.screen);
}
void app_attach_tests(void) {
    screen_fixture_prepare();
    test_run("attachment selection and read allow an open writer", selection_and_read_allow_open_writer);
    test_run("attachment metadata boundary and grouped refusals", metadata_boundary_and_grouped_refusals);
    test_run("attachment selections serialize and read current bytes", serialized_selections_read_current_bytes);
    test_run("removed active and queued attachment chips never return", removed_active_and_queued_chips_never_return);
    test_run("attachment cancellation at each worker boundary", cancellation_at_each_worker_boundary);
    test_run("attachments clear on scope account and endpoint changes", owner_scope_account_and_endpoint_changes);
    test_run("queued reads revalidate file kind and size", queued_read_revalidates_file_kind_and_size);
    test_run("pasted image encoding keeps PNG support", pasted_image_encoding_keeps_png_support);
    test_run("conversation gates pending attachment selection", conversation_gates_pending_selection);
    app_attach_initial_tests();
    app_attach_read_probe_tests();
}
