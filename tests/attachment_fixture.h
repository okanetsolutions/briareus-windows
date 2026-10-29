#ifndef BRIAREUS_TEST_ATTACHMENT_FIXTURE_H
#define BRIAREUS_TEST_ATTACHMENT_FIXTURE_H
#include "screen_fixture.h"
#include "attach_list_internal.h"
static Route attachment_routes[] = {{"POST", "/uploads", "manage"}, {"POST", "/sessions", "manage"},
    {"POST", "/sessions/{id}/messages", "manage"}};

typedef struct {
    ScreenFixture screen;
    volatile LONG calls;
    bool block_first;
    size_t sizes[4]; unsigned char first[4];
} UploadFixture;
static inline bool upload_transport(void *ctx, const char *method, const char *url, const char *const *headers,
    const void *body, size_t bytes, int timeout, int *status, char **type, char **retry,
    char **response, size_t *len, char **error) {
    (void)headers; (void)timeout; (void)retry; (void)error;
    UploadFixture *f = ctx;
    LONG index = InterlockedIncrement(&f->calls) - 1;
    CHECK_STR(method, "POST"); CHECK(strstr(url, "/uploads?name=") != NULL);
    CHECK(index < 4);
    if (index < 4) { f->sizes[index] = bytes; f->first[index] = bytes ? *(const unsigned char *)body : 0; }
    if (index == 0 && f->block_first) { SetEvent(f->screen.entered); CHECK_INT(WaitForSingleObject(f->screen.resume, 10000), WAIT_OBJECT_0); }
    *status = 201; *type = xstrdup("application/json");
    *response = xstrfmt("{\"file\":{\"id\":\"file-%ld\"}}", index + 1); *len = strlen(*response);
    return true;
}
static inline void init(UploadFixture *f) {
    memset(f, 0, sizeof *f); screen_fixture_init(&f->screen, attachment_routes, sizeof attachment_routes / sizeof *attachment_routes);
    api_client_set_transport(f->screen.client, upload_transport, f);
}
static inline void pump(UploadFixture *f, UINT message) {
    ULONGLONG until = GetTickCount64() + 10000; MSG msg;
    while (GetTickCount64() < until) {
        if (PeekMessageW(&msg, f->screen.dispatcher, message, message, PM_REMOVE)) {
            store_handle_message(message, msg.wParam, msg.lParam); return;
        }
        Sleep(1);
    }
    CHECK(false);
}
static inline void temp(wchar_t *path) {
    wchar_t dir[MAX_PATH]; CHECK(GetTempPathW(MAX_PATH, dir) != 0);
    CHECK(GetTempFileNameW(dir, L"att", 0, path) != 0);
}
static inline void write_file(const wchar_t *path, const char *bytes) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); DWORD written = 0;
    CHECK(WriteFile(file, bytes, (DWORD)strlen(bytes), &written, NULL)); CHECK_INT(written, strlen(bytes)); CloseHandle(file);
}
static inline void resize_file(const wchar_t *path, LONGLONG length) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); LARGE_INTEGER size; size.QuadPart = length;
    CHECK(SetFilePointerEx(file, size, NULL, FILE_BEGIN)); CHECK(SetEndOfFile(file)); CloseHandle(file);
}
static inline void select_paths(AttachList *l, wchar_t paths[][MAX_PATH], size_t count) {
    const wchar_t *p[12] = {0}; CHECK(count <= 12);
    if (!count || count > 12) return;
    for (size_t i = 0; i < count; i++) p[i] = paths[i];
    attach_list_paths(l, p, count);
}
static inline void complete_file(UploadFixture *f) { pump(f, WM_APP_ASYNC_DONE); pump(f, WM_APP_REQUEST_DONE); }

#endif
