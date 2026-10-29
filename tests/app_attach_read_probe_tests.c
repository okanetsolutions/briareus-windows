// Exercise the public reader with a real disk handle, stopping exactly after its first chunk.
#include "screen_fixture.h"
#include <windows.h>
#include "test.h"
#include "suites.h"
#include <stdlib.h>

static bool fail_schedule;
static bool observed_schedule(AsyncWork work, AsyncWork done, void *ctx) {
    return !fail_schedule && async_try_run(work, done, ctx);
}
#define async_try_run observed_schedule
static volatile LONG *stop_after_chunk;
static DWORD largest_read;
static int chunk_reads;
static BOOL WINAPI observed_read(HANDLE file, LPVOID bytes, DWORD count, LPDWORD got, LPOVERLAPPED overlapped) {
    chunk_reads++; if (count > largest_read) largest_read = count;
    BOOL ok = ReadFile(file, bytes, count, got, overlapped);
    if (stop_after_chunk) InterlockedExchange(stop_after_chunk, 1);
    return ok;
}
#define ReadFile observed_read
#define attacher_new probe_attacher_new
#define attacher_free probe_attacher_free
#define attacher_pending probe_attacher_pending
#define attach_file_clear probe_attach_file_clear
#define attach_files_free probe_attach_files_free
#define attach_file_read probe_attach_file_read
#define attach_clipboard_has_files probe_attach_clipboard_has_files
#define attach_clipboard_has_text probe_attach_clipboard_has_text
#define attacher_from_clipboard probe_attacher_from_clipboard
#define attacher_from_drop probe_attacher_from_drop
#define attacher_from_paths probe_attacher_from_paths
#define format_file_size probe_format_file_size
#include "../app/attach.c"

// Compile the real entrypoints with a failing scheduler and a modal that retires its owner.
#define attach_list_new probe_attach_list_new
#define attach_list_free probe_attach_list_free
#define attach_list_supported probe_attach_list_supported
#define attach_list_paste probe_attach_list_paste
#define attach_list_drop probe_attach_list_drop
#define attach_list_pick probe_attach_list_pick
#define attach_list_pick_files probe_attach_list_pick_files
#define attach_list_paths probe_attach_list_paths
#define attach_list_remove probe_attach_list_remove
#define attach_list_scope probe_attach_list_scope
#define attach_lists_disconnect probe_attach_lists_disconnect
#define attach_list_count probe_attach_list_count
#define attach_list_uploading probe_attach_list_uploading
#define attach_list_ids probe_attach_list_ids
#define attach_list_sent probe_attach_list_sent
#define attach_list_height probe_attach_list_height
#define attach_list_paint probe_attach_list_paint
#define attach_list_click probe_attach_list_click
#define attach_button_paint probe_attach_button_paint
#define attach_list_changed probe_attach_list_changed
#define attach_list_attacher probe_attach_list_attacher
#define attach_list_sync probe_attach_list_sync
#define app_alert probe_app_alert
void probe_app_alert(const char *title, const char *message);
#include "attachment_fixture.h"
#include "../app/attach_queue.c"
#include <commdlg.h>
static int picker_depth, picker_begins, picker_ends;
static void picker_modal_begin(HWND hwnd) { picker_depth++; picker_begins++; app_modal_begin(hwnd); }
static void picker_modal_end(void) { CHECK_INT(picker_depth, 1); picker_depth--; picker_ends++; app_modal_end(); }
static DWORD WINAPI picker_error(void) { return 0; }
#define app_modal_begin picker_modal_begin
#define app_modal_end picker_modal_end
#define CommDlgExtendedError picker_error
static BOOL WINAPI observed_picker(LPOPENFILENAMEW args);
#define GetOpenFileNameW observed_picker
#include "../app/attach_list.c"

static AttachList *failed_owner;
static int failure_alerts;
static int picker_change;
static bool capture_notices;
static char *notice;
void probe_app_alert(const char *title, const char *message) {
    (void)title; CHECK(message && *message); failure_alerts++;
    if (capture_notices) { free(notice); notice = xstrdup(message); return; }
    attach_list_free(failed_owner); failed_owner = NULL;
}
static size_t notice_lines(void) {
    size_t lines = notice ? 1 : 0;
    for (const char *c = notice; c && *c; c++) if (*c == '\n') lines++;
    return lines;
}
// Rejection behavior uses captured feedback: a real MessageBox would introduce an unbounded modal wait in CI.
static void count_limit_uses_current_reserved_slots(void) {
    UploadFixture f; init(&f); f.block_first = true; Screen screen = {0};
    AttachList *l = attach_list_new(&screen, "message"); wchar_t path[MAX_PATH], empty[MAX_PATH];
    temp(path); temp(empty); write_file(path, "x"); capture_notices = true; failure_alerts = 0;
    const wchar_t *paths[12]; for (size_t i = 0; i < 11; i++) paths[i] = path; paths[11] = empty;
    attach_list_paths(l, paths, 12); pump(&f, WM_APP_ASYNC_DONE);
    CHECK_INT(attach_list_count(l), 10); CHECK(attach_list_uploading(l));
    CHECK_INT(failure_alerts, 1); CHECK_INT(notice_lines(), 2);
    pump(&f, WM_APP_ASYNC_DONE); CHECK_INT(WaitForSingleObject(f.screen.entered, 10000), WAIT_OBJECT_0);
    attach_list_paths(l, paths, 1); pump(&f, WM_APP_ASYNC_DONE);
    CHECK_INT(attach_list_count(l), 10); CHECK_INT(f.calls, 1);
    CHECK_INT(failure_alerts, 2); CHECK_INT(notice_lines(), 1);
    attach_list_free(l); SetEvent(f.screen.resume); pump(&f, WM_APP_REQUEST_DONE);
    capture_notices = false; free(notice); notice = NULL;
    DeleteFileW(path); DeleteFileW(empty); screen_fixture_clear(&f.screen);
}
static BOOL WINAPI observed_picker(LPOPENFILENAMEW args) {
    CHECK_INT(picker_depth, 1);
    if (picker_change == 3) return FALSE;
    if (picker_change == 0) { attach_list_free(failed_owner); failed_owner = NULL; }
    else if (picker_change == 1) attach_list_scope(failed_owner, "o/replacement");
    else g_store.client = NULL;
    wcscpy(args->lpstrFile, L"must-not-be-selected");
    return TRUE;
}
static void picker_discards_results_after_context_replacement(void) {
    Route routes[] = {{"POST", "/uploads", "manage"}, {"POST", "/sessions", "manage"}};
    for (picker_change = 0; picker_change < 4; picker_change++) {
        ScreenFixture f; screen_fixture_init(&f, routes, 2); Screen screen = {0};
        failed_owner = attach_list_new(&screen, "start_session");
        fail_schedule = true; failure_alerts = picker_begins = picker_ends = 0;
        attach_list_pick(failed_owner);
        CHECK_INT(failure_alerts, 0); CHECK_INT(f.writes, 0);
        CHECK_INT(picker_begins, 1); CHECK_INT(picker_ends, 1); CHECK_INT(picker_depth, 0);
        if (failed_owner) { CHECK_INT(attach_list_count(failed_owner), 0); attach_list_free(failed_owner); failed_owner = NULL; }
        fail_schedule = false; g_store.client = f.client; screen_fixture_clear(&f);
    }
}
static void failed_selection_worker_survives_owner_retirement(void) {
    Route routes[] = {{"POST", "/uploads", "manage"}, {"POST", "/sessions", "manage"}};
    ScreenFixture f; screen_fixture_init(&f, routes, 2); Screen screen = {0};
    failed_owner = attach_list_new(&screen, "start_session"); fail_schedule = true; failure_alerts = 0;
    const wchar_t *path = L"not-read-on-ui-thread";
    attach_list_paths(failed_owner, &path, 1);
    CHECK(failed_owner == NULL); CHECK_INT(failure_alerts, 1); CHECK_INT(f.writes, 0);
    fail_schedule = false; screen_fixture_clear(&f);
}

static void bounded_reads_stop_on_cancellation(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, dir) != 0);
    CHECK(GetTempFileNameW(dir, L"arc", 0, path) != 0);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(h != INVALID_HANDLE_VALUE); LARGE_INTEGER size; size.QuadPart = 1024 * 1024 + 7;
    CHECK(SetFilePointerEx(h, size, NULL, FILE_BEGIN)); CHECK(SetEndOfFile(h)); CloseHandle(h);
    AttachFile file = {.name = xstrdup("chunks"), .path = _wcsdup(path)}; volatile LONG stop = 0;
    stop_after_chunk = &stop; chunk_reads = 0; largest_read = 0;
    char *error = attach_file_read(&file, &stop); free(error);
    CHECK_INT(chunk_reads, 1); CHECK_INT(largest_read, 1048576); CHECK(file.bytes == NULL);
    stop_after_chunk = NULL; InterlockedExchange(&stop, 0); chunk_reads = 0;
    CHECK(attach_file_read(&file, &stop) == NULL); CHECK_INT(chunk_reads, 2); CHECK_INT(file.len, 1048583);
    CHECK_INT(largest_read, 1048576); attach_file_clear(&file); DeleteFileW(path);
}
void app_attach_read_probe_tests(void) {
    test_run("attachment count limit counts reserved chips with grouped feedback", count_limit_uses_current_reserved_slots);
    test_run("attachment reads bounded chunks and stop during read", bounded_reads_stop_on_cancellation);
    test_run("failed attachment worker survives modal owner retirement", failed_selection_worker_survives_owner_retirement);
    test_run("attachment picker discards results after context replacement", picker_discards_results_after_context_replacement);
}
