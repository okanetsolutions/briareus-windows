#ifndef BRIAREUS_TEST_SCREEN_FIXTURE_H
#define BRIAREUS_TEST_SCREEN_FIXTURE_H
#include "screens.h"
#include "str.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    ApiClient *client;
    HWND dispatcher, parent;
    Pane *pane;
    const char *read_response, *write_response;
    int read_status, write_status;
    volatile LONG reads, writes;
    char *method, *url;
    Json *body;
    HANDLE entered, resume;
    bool block_read, block_write;
} ScreenFixture;

static inline bool screen_fixture_transport(void *ctx, const char *method, const char *url,
    const char *const *headers, const void *body, size_t bytes, int timeout, int *status,
    char **type, char **retry, char **response, size_t *len, char **error) {
    (void)headers; (void)timeout;
    ScreenFixture *f = ctx;
    bool read = str_eq(method, "GET");
    if (read) InterlockedIncrement(&f->reads);
    else {
        free(f->method); free(f->url); json_free(f->body);
        f->method = xstrdup(method); f->url = xstrdup(url);
        char *text = xstrndup(body ? body : "", bytes);
        f->body = json_parsez(text); free(text);
        InterlockedIncrement(&f->writes);
    }
    if ((read && f->block_read) || (!read && f->block_write)) {
        SetEvent(f->entered); WaitForSingleObject(f->resume, 10000);
    }
    *status = read ? f->read_status : f->write_status;
    *type = xstrdup("application/json"); *retry = NULL; *error = NULL;
    const char *answer = read ? f->read_response : f->write_response;
    *response = xstrdup(answer ? answer : "{}"); *len = strlen(*response);
    return true;
}

static inline void screen_fixture_prepare(void) {
    if (!theme.canvas) theme_init();
    // Warm process-lifetime drawing registries outside the per-test leak snapshot.
    HWND parent = CreateWindowExW(0, L"STATIC", L"Screen test startup", 0,
        0, 0, 10, 10, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(parent != NULL); Pane *pane = pane_create(parent, false);
    CHECK(pane != NULL); pane_destroy(pane); DestroyWindow(parent);
}

static inline void screen_fixture_init(ScreenFixture *f, Route *routes, size_t count) {
    memset(f, 0, sizeof *f); memset(&g_store, 0, sizeof g_store);
    if (!theme.canvas) theme_init();
    f->read_status = f->write_status = 200;
    f->entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    f->resume = CreateEventW(NULL, TRUE, FALSE, NULL);
    f->dispatcher = CreateWindowExW(0, L"STATIC", L"Screen test dispatcher", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(f->dispatcher != NULL);
    g_store.hwnd = f->dispatcher; g_store.has_device = g_store.active = true;
    g_store.device.permission = "admin"; g_store.routes = routes; g_store.route_count = count;
    ServerAddress address; ApiError error; api_error_init(&error);
    CHECK(server_address_parse("https://example.com", &address));
    f->client = api_client_new(&address, "brm_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &error);
    CHECK(f->client != NULL); server_address_free(&address); api_error_clear(&error);
    g_store.client = f->client; api_client_set_transport(f->client, screen_fixture_transport, f);
}

static inline void screen_fixture_pump(ScreenFixture *f, int expected) {
    ULONGLONG end = GetTickCount64() + 10000; int delivered = 0;
    while (delivered < expected && GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, f->dispatcher, WM_APP_REQUEST_DONE, WM_APP_REQUEST_DONE, PM_REMOVE)) {
            store_handle_message(msg.message, msg.wParam, msg.lParam); delivered++;
        }
        if (delivered < expected) Sleep(1);
    }
    CHECK_INT(delivered, expected);
}

static inline void screen_fixture_pane(ScreenFixture *f, Screen *s) {
    f->parent = CreateWindowExW(0, L"STATIC", L"Screen test controls", WS_POPUP,
        0, 0, 900, 2000, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(f->parent != NULL); f->pane = pane_create(f->parent, false); CHECK(f->pane != NULL);
    RECT bounds = {0, 0, 900, 2000}; pane_set_bounds(f->pane, &bounds);
    ShowWindow(f->parent, SW_SHOWNA); pane_show(f->pane, true); s->pane = f->pane;
}

static inline void screen_fixture_clear(ScreenFixture *f) {
    if (f->pane) pane_destroy(f->pane);
    if (f->parent) DestroyWindow(f->parent);
    api_client_release(f->client); DestroyWindow(f->dispatcher);
    CloseHandle(f->entered); CloseHandle(f->resume);
    free(f->method); free(f->url); json_free(f->body); memset(&g_store, 0, sizeof g_store);
}

static inline void screen_fixture_draw(Screen *s, Doc *doc) {
    doc_begin(doc, NULL, 800); s->vt->layout(s, doc); doc_end(doc);
}

static inline int screen_fixture_find(Doc *doc, const char *text) {
    for (size_t i = 0; i < doc->count; i++) if (str_eq(doc->items[i].text, text) || str_eq(doc->items[i].tip, text)) return (int)i;
    return -1;
}

static inline void screen_fixture_click(Screen *s, Doc *doc, const char *text) {
    screen_fixture_draw(s, doc); int i = screen_fixture_find(doc, text); CHECK(i >= 0);
    if (i >= 0) { CHECK(doc->items[i].action != 0); s->vt->action(s, doc->items[i].action, doc->items[i].arg, (POINT){0}); }
}

static inline void screen_fixture_select(Screen *s, Doc *doc, const char *subject) {
    screen_fixture_draw(s, doc); int i = screen_fixture_find(doc, subject); CHECK(i >= 0);
    if (i < 0) return;
    RECT rc = doc->items[i].rc;
    int hit = doc_hit(doc, (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2); CHECK(hit >= 0);
    if (hit >= 0) {
        Item *row = &doc->items[hit]; CHECK(row->action != 0);
        s->vt->action(s, row->action, row->arg, (POINT){0});
    }
}
#endif
