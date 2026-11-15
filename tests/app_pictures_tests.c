#include "screen_fixture.h"
#include "screen_conversation_internal.h"
#include "image_cache.h"
#include "picture.h"
#include "remote_image.h"
#include "suites.h"

// 4 × 2, RGBA: red, green, blue, white over half-transparent red, transparent, black, grey.
static const unsigned char PNG_4X2[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x7F, 0xA8, 0x7D, 0x63, 0x00, 0x00, 0x00, 0x1D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63,
    0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x0C, 0x19, 0xFE, 0x83, 0x01, 0x88, 0xD5, 0xC0, 0x00, 0x01, 0xFF, 0x1B, 0x1A, 0x1A, 0xFE, 0x03, 0x00,
    0x10, 0x2B, 0x0E, 0xF4, 0x17, 0x51, 0x7C, 0xCF, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
static const char OWN[] = "https://example.com/files/ab/shot.png";   // on the fixture's paired origin
static const char OTHER[] = "https://cdn.test/a.png";

/// The pictures' network: counts the downloads, and holds them on `hold` when it is set.
typedef struct { volatile LONG calls; int status; HANDLE hold; } Net;

static bool net_transport(void *ctx, const char *method, const char *url, const char *const *headers, const void *body,
                          size_t body_len, int timeout_ms, int *status, char **content_type, char **retry_after,
                          char **response, size_t *response_len, char **error_message) {
    (void)method; (void)url; (void)headers; (void)body; (void)body_len; (void)timeout_ms;
    Net *net = ctx;
    InterlockedIncrement(&net->calls);
    if (net->hold) WaitForSingleObject(net->hold, 10000);
    *status = net->status; *content_type = NULL; *retry_after = NULL; *error_message = NULL;
    *response = xmalloc(sizeof PNG_4X2); memcpy(*response, PNG_4X2, sizeof PNG_4X2); *response_len = sizeof PNG_4X2;
    return true;
}

static int changes;
static void count_change(void *ctx) { (void)ctx; changes++; }

/// Delivers the workers' completions for a while.
static void deliver(DWORD ms) {
    ULONGLONG until = GetTickCount64() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, g_store.hwnd, WM_APP_ASYNC_DONE, WM_APP_ASYNC_DONE, PM_REMOVE)) store_handle_message(msg.message, msg.wParam, msg.lParam);
        Sleep(1);
    } while (GetTickCount64() < until);
}
/// Delivers completions until the picture leaves IMAGE_LOADING; the state it then has.
static ImageState loaded_state(const char *url) {
    ULONGLONG until = GetTickCount64() + 10000;
    while (image_cache_get(url).state == IMAGE_LOADING && GetTickCount64() < until) deliver(1);
    return image_cache_get(url).state;
}
static void start(ScreenFixture *f, Net *net) {
    screen_fixture_init(f, NULL, 0);
    memset(net, 0, sizeof *net); net->status = 200;
    remote_image_set_transport(net_transport, net);
    changes = 0;
}
static void finish(ScreenFixture *f) {
    image_cache_reset(); image_cache_set_limits(0, 0); remote_image_set_transport(NULL, NULL);
    screen_fixture_clear(f);
}

/// Saved responses in a directory of their own, as a connection has; `directory` takes MAX_PATH characters.
static void open_saved_responses(wchar_t *directory) {
    wchar_t temp[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp) != 0);
    CHECK(GetTempFileNameW(temp, L"pic", 0, directory) != 0); DeleteFileW(directory);
    CHECK(CreateDirectoryW(directory, NULL)); g_store.cache = cache_new(directory);
}
static void close_saved_responses(const wchar_t *directory) {
    cache_remove_all(g_store.cache); cache_free(g_store.cache); g_store.cache = NULL;
    RemoveDirectoryW(directory);
}

static void pictures_decode_premultiplied_and_scaled(void) {
    Picture p; int fw = 0, fh = 0;
    CHECK(picture_decode(PNG_4X2, sizeof PNG_4X2, 0, 0, &p, &fw, &fh));
    CHECK_INT(p.width, 4); CHECK_INT(p.height, 2); CHECK_INT(fw, 4); CHECK_INT(fh, 2); CHECK(p.key != 0);
    if (p.pixels) {
        static const unsigned char red[] = { 0, 0, 255, 255 }, half_red[] = { 0, 0, 128, 128 }, clear[] = { 0, 0, 0, 0 }, grey[] = { 128, 128, 128, 255 };
        CHECK(memcmp(p.pixels, red, 4) == 0);
        CHECK(memcmp(p.pixels + 4 * 4, half_red, 4) == 0);
        CHECK(memcmp(p.pixels + 5 * 4, clear, 4) == 0);
        CHECK(memcmp(p.pixels + 7 * 4, grey, 4) == 0);
    }
    Picture scaled; int sw = 0, sh = 0;
    CHECK(picture_decode(PNG_4X2, sizeof PNG_4X2, 2, 2, &scaled, &sw, &sh));
    CHECK_INT(scaled.width, 2); CHECK_INT(scaled.height, 1); CHECK_INT(sw, 4); CHECK_INT(sh, 2);
    CHECK(scaled.key != 0 && scaled.key != p.key);
    picture_free(&p); picture_free(&scaled);
    CHECK(p.pixels == NULL);
    Picture none; int nw = 9, nh = 9;
    CHECK(!picture_decode("<!doctype html><html></html>", 28, 0, 0, &none, &nw, &nh));
    CHECK(none.pixels == NULL); CHECK_INT(nw, 0); CHECK_INT(nh, 0);
    CHECK(!picture_decode(PNG_4X2, 40, 0, 0, &none, &nw, &nh));
    CHECK(!picture_decode(NULL, 0, 0, 0, &none, &nw, &nh));
    int w = 0, h = 0;
    picture_fit(1920, 1080, 480, 360, &w, &h); CHECK_INT(w, 480); CHECK_INT(h, 270);
    picture_fit(100, 1000, 480, 360, &w, &h); CHECK_INT(w, 36); CHECK_INT(h, 360);
    picture_fit(100, 50, 480, 360, &w, &h); CHECK_INT(w, 100); CHECK_INT(h, 50);
    picture_fit(4000, 1, 400, 0, &w, &h); CHECK_INT(w, 400); CHECK_INT(h, 1);
}

static void another_hosts_picture_waits_for_the_users_word(void) {
    ScreenFixture f; Net net; start(&f, &net);
    image_cache_watch(count_change, &net);
    ImageInfo info = image_cache_get(OTHER);
    CHECK_INT(info.state, IMAGE_ASK); CHECK(!info.host_allowed); CHECK(info.preview == NULL);
    deliver(50);
    CHECK_INT(net.calls, 0); CHECK(image_cache_shown(OTHER) == NULL);
    size_t len = 9; CHECK(image_cache_bytes(OTHER, &len) == NULL); CHECK_INT(len, 0);
    image_cache_allow(OTHER);
    CHECK_INT(loaded_state(OTHER), IMAGE_READY); CHECK_INT(net.calls, 1); CHECK(changes >= 2);
    info = image_cache_get(OTHER);
    CHECK(info.preview != NULL); CHECK_INT(info.width, 4); CHECK_INT(info.height, 2); CHECK_INT(info.bytes, sizeof PNG_4X2);
    CHECK_INT(info.preview_w, 4); CHECK_INT(info.preview_h, 2);
    if (info.preview) { CHECK_INT(info.preview->width, 4); CHECK_INT(info.preview->height, 2); CHECK(info.preview == image_cache_shown(OTHER)); }
    void *bytes = image_cache_bytes(OTHER, &len);
    CHECK_INT(len, sizeof PNG_4X2); CHECK(bytes && memcmp(bytes, PNG_4X2, sizeof PNG_4X2) == 0); free(bytes);
    // The host was allowed, not the one address; any other host still waits.
    CHECK_INT(image_cache_get("https://cdn.test/b.png").state, IMAGE_LOADING);
    CHECK_INT(loaded_state("https://cdn.test/b.png"), IMAGE_READY); CHECK_INT(net.calls, 2);
    CHECK_INT(image_cache_get("https://cdn.test.evil.test/b.png").state, IMAGE_ASK);
    CHECK_INT(image_cache_get("https://cdn.test:8443/b.png").state, IMAGE_ASK);
    // Nothing but HTTPS is fetched, asked for or not.
    image_cache_allow("http://plain.test/a.png");
    CHECK_INT(image_cache_get("http://plain.test/a.png").state, IMAGE_ASK);
    deliver(50); CHECK_INT(net.calls, 2);
    // Without saved responses to keep it in, a changed connection forgets the host with the pictures.
    int before = changes;
    image_cache_reset();
    CHECK(changes > before); CHECK(image_cache_shown(OTHER) == NULL);
    info = image_cache_get(OTHER); CHECK_INT(info.state, IMAGE_ASK); CHECK(!info.host_allowed);
    image_cache_unwatch(&net);
    before = changes; image_cache_allow(OTHER); CHECK_INT(loaded_state(OTHER), IMAGE_READY); CHECK_INT(changes, before);
    finish(&f);
}

static void an_allowed_host_lasts_as_long_as_the_connections_saved_responses(void) {
    ScreenFixture f; Net net; start(&f, &net);
    wchar_t directory[MAX_PATH]; open_saved_responses(directory);
    CHECK_INT(image_cache_get(OTHER).state, IMAGE_ASK);
    image_cache_allow(OTHER);
    CHECK_INT(loaded_state(OTHER), IMAGE_READY); CHECK_INT(net.calls, 1);
    // The pictures go with the app or a reconnection; the host is read back from what the connection saved.
    image_cache_reset();
    CHECK(image_cache_shown(OTHER) == NULL);
    CHECK_INT(image_cache_get(OTHER).state, IMAGE_LOADING);
    CHECK_INT(loaded_state(OTHER), IMAGE_READY); CHECK_INT(net.calls, 2);
    CHECK_INT(image_cache_get("https://elsewhere.test/a.png").state, IMAGE_ASK);
    // Forgetting the connection erases what it saved, and the host with it.
    image_cache_reset(); cache_remove_all(g_store.cache);
    ImageInfo info = image_cache_get(OTHER); CHECK_INT(info.state, IMAGE_ASK); CHECK(!info.host_allowed);
    deliver(50); CHECK_INT(net.calls, 2);
    close_saved_responses(directory);
    finish(&f);
}

static void the_paired_servers_pictures_load_and_a_failure_is_retried_on_request(void) {
    ScreenFixture f; Net net; start(&f, &net);
    net.status = 503;
    CHECK_INT(image_cache_get(OWN).state, IMAGE_LOADING);
    CHECK_INT(loaded_state(OWN), IMAGE_FAILED); CHECK_INT(net.calls, 1);
    ImageInfo info = image_cache_get(OWN);
    CHECK(info.error && *info.error); CHECK(info.preview == NULL);
    // A failed picture is not fetched again by a layout asking for it.
    deliver(50); CHECK_INT(image_cache_get(OWN).state, IMAGE_FAILED); CHECK_INT(net.calls, 1);
    net.status = 200;
    image_cache_retry(OWN);
    CHECK_INT(loaded_state(OWN), IMAGE_READY); CHECK_INT(net.calls, 2);
    image_cache_retry(OWN); image_cache_retry_failed(); deliver(50); CHECK_INT(net.calls, 2);
    // Reading a screen again retries every picture that failed, and only those.
    net.status = 404;
    CHECK_INT(loaded_state("https://example.com/files/ab/one.png"), IMAGE_FAILED);
    CHECK_INT(loaded_state("https://example.com/files/ab/two.png"), IMAGE_FAILED); CHECK_INT(net.calls, 4);
    net.status = 200;
    image_cache_retry_failed();
    CHECK_INT(loaded_state("https://example.com/files/ab/one.png"), IMAGE_READY);
    CHECK_INT(loaded_state("https://example.com/files/ab/two.png"), IMAGE_READY); CHECK_INT(net.calls, 6);
    CHECK_INT(image_cache_get("https://example.com.evil.test/files/ab/shot.png").state, IMAGE_ASK);
    deliver(50); CHECK_INT(net.calls, 6);
    finish(&f);
}

/// Delivers completions until the picture's preview is held; false when that takes too long.
static bool held(const char *url) {
    ULONGLONG until = GetTickCount64() + 10000;
    while (!image_cache_get(url).preview && GetTickCount64() < until) deliver(1);
    return image_cache_get(url).preview != NULL;
}

static void pictures_out_of_view_give_way_and_come_back_when_shown(void) {
    static const char A[] = "https://example.com/files/ab/a.png", B[] = "https://example.com/files/ab/b.png", C[] = "https://example.com/files/ab/c.png";
    ScreenFixture f; Net net; start(&f, &net);
    // Room for two of these files with their previews, and nothing counts as in view but the picture that just arrived.
    image_cache_set_limits(300, 0);
    CHECK_INT(loaded_state(A), IMAGE_READY); CHECK_INT(loaded_state(B), IMAGE_READY);
    CHECK(image_cache_get(A).preview != NULL); CHECK(image_cache_get(B).preview != NULL);
    CHECK_INT(loaded_state(C), IMAGE_READY); CHECK_INT(net.calls, 3);
    // The first gave way: a layout still has its size, and nothing holds its pixels or its file.
    ImageInfo info = image_cache_get(A);
    CHECK_INT(info.state, IMAGE_READY); CHECK(info.preview == NULL); CHECK_INT(info.preview_w, 4); CHECK_INT(info.preview_h, 2);
    CHECK_INT(info.width, 4); CHECK_INT(info.bytes, 0);
    size_t len = 9; CHECK(image_cache_bytes(A, &len) == NULL); CHECK_INT(len, 0);
    CHECK(image_cache_get(B).preview != NULL); CHECK(image_cache_get(C).preview != NULL);
    // Asking for it in a layout fetches nothing; painting it does, once, and the one longest unseen gives way for it.
    deliver(50); CHECK_INT(net.calls, 3);
    CHECK(image_cache_shown(A) == NULL); CHECK(image_cache_shown(A) == NULL);
    CHECK_INT(image_cache_get(A).state, IMAGE_READY);
    CHECK(held(A)); CHECK_INT(net.calls, 4);
    CHECK(image_cache_shown(A) != NULL);
    CHECK(image_cache_get(B).preview == NULL); CHECK(image_cache_get(C).preview != NULL);
    finish(&f);
}

static void a_download_under_way_is_dropped_when_the_connection_goes(void) {
    ScreenFixture f; Net net; start(&f, &net);
    net.hold = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK_INT(image_cache_get(OWN).state, IMAGE_LOADING);
    ULONGLONG until = GetTickCount64() + 10000;
    while (!net.calls && GetTickCount64() < until) Sleep(1);
    image_cache_reset();
    SetEvent(net.hold); deliver(300);
    CHECK_INT(net.calls, 1); CHECK(image_cache_shown(OWN) == NULL);
    CHECK_INT(loaded_state(OWN), IMAGE_READY); CHECK_INT(net.calls, 2);
    CloseHandle(net.hold);
    finish(&f);
}

static Route ROUTES[] = {{"GET", "/sessions/{sessionId}", "read"}};

static int item_with_action(Doc *doc, int action) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].action == action) return (int)i;
    return -1;
}

/// The viewer's page has one thing to click: the picture.
static int clickable_item(Doc *doc) {
    for (size_t i = 0; i < doc->count; i++) if (doc->items[i].action) return (int)i;
    return -1;
}

static void a_replys_pictures_follow_it_and_open_in_the_viewer(void) {
    ScreenFixture f; Net net; start(&f, &net);
    g_store.routes = ROUTES; g_store.route_count = 1;
    wchar_t directory[MAX_PATH]; open_saved_responses(directory);
    Session session = {0}; session.raw = json_parsez("{\"id\":\"pictures\",\"status\":\"idle\"}");
    Screen *screen = conversation_screen_new(&session); session_free(&session);
    screen_fixture_pane(&f, screen); screen->vt->visible(screen, true);
    f.read_response = "{\"session\":{\"id\":\"pictures\",\"status\":\"idle\"},\"events\":[{\"seq\":1,\"kind\":\"text\",\"text\":"
        "\"Done: [shot.png](<https://example.com/files/ab/shot.png>) beside ![theirs](https://cdn.test/a.png) and `![code](https://example.com/files/ab/code.png)`\"}]}";
    screen_conversation_refresh((ConversationScreen *)screen, false); screen_fixture_pump(&f, 1);
    Doc doc; doc_init(&doc);
    screen_fixture_draw(screen, &doc);
    // The paired server's picture is on its way; the other host's waits to be asked for, and code is no picture.
    CHECK(item_with_action(&doc, ACT_PICTURE_OPEN) < 0);
    int ask = item_with_action(&doc, ACT_PICTURE_ALLOW); CHECK(ask >= 0);
    if (ask >= 0) CHECK_STR((const char *)doc.items[ask].arg, OTHER);
    CHECK_INT(loaded_state(OWN), IMAGE_READY); CHECK_INT(net.calls, 1);
    screen_fixture_draw(screen, &doc);
    int open = item_with_action(&doc, ACT_PICTURE_OPEN); CHECK(open >= 0);
    CHECK(item_with_action(&doc, ACT_PICTURE_ALLOW) > open);
    if (open >= 0) {
        CHECK_STR((const char *)doc.items[open].arg, OWN);
        screen->vt->action(screen, doc.items[open].action, doc.items[open].arg, (POINT){0});
    }
    Screen *viewer = pane_top(f.pane); CHECK(viewer != NULL && viewer != screen);
    if (viewer && viewer != screen) {
        CHECK_STR(viewer->id, "image:https://example.com/files/ab/shot.png");
        Doc page; doc_init(&page);
        ULONGLONG until = GetTickCount64() + 10000; int shown = -1;
        while (shown < 0 && GetTickCount64() < until) { deliver(1); screen_fixture_draw(viewer, &page); shown = clickable_item(&page); }
        // The whole picture at its own size, which the pane has room for; a zoom step is a quarter more.
        CHECK(shown >= 0);
        if (shown >= 0) { CHECK_INT(page.items[shown].rc.right - page.items[shown].rc.left, 4); CHECK_INT(page.items[shown].rc.bottom - page.items[shown].rc.top, 2); }
        CHECK(viewer->vt->key(viewer, VK_ADD, false, false));
        screen_fixture_draw(viewer, &page); shown = clickable_item(&page);
        if (shown >= 0) CHECK_INT(page.items[shown].rc.right - page.items[shown].rc.left, 5);
        CHECK(viewer->vt->key(viewer, '0', false, false));
        screen_fixture_draw(viewer, &page); shown = clickable_item(&page);
        if (shown >= 0) CHECK_INT(page.items[shown].rc.right - page.items[shown].rc.left, 4);
        HeaderInfo header = {0}; viewer->vt->header(viewer, &header);
        CHECK_STR(header.title, "shot.png");
        doc_free(&page);
    }
    pane_set_root(f.pane, NULL);
    // Asking for the other host's picture fetches it, and the conversation shows it at its next layout.
    screen->pane = f.pane;
    screen_fixture_draw(screen, &doc);
    ask = item_with_action(&doc, ACT_PICTURE_ALLOW); CHECK(ask >= 0);
    if (ask >= 0) screen->vt->action(screen, doc.items[ask].action, doc.items[ask].arg, (POINT){0});
    CHECK_INT(loaded_state(OTHER), IMAGE_READY); CHECK_INT(net.calls, 2);
    screen_fixture_draw(screen, &doc);
    CHECK(item_with_action(&doc, ACT_PICTURE_ALLOW) < 0);
    doc_free(&doc); screen->vt->destroy(screen);
    // A closed conversation no longer hears of the pictures.
    image_cache_reset();
    close_saved_responses(directory); finish(&f);
}

void app_pictures_tests(void) {
    screen_fixture_prepare();
    test_run("pictures decode premultiplied and scaled to fit", pictures_decode_premultiplied_and_scaled);
    test_run("another host's picture waits for the user's word", another_hosts_picture_waits_for_the_users_word);
    test_run("an allowed host lasts as long as the connection's saved responses", an_allowed_host_lasts_as_long_as_the_connections_saved_responses);
    test_run("the paired server's pictures load, and a failure is retried on request", the_paired_servers_pictures_load_and_a_failure_is_retried_on_request);
    test_run("pictures out of view give way and come back when shown", pictures_out_of_view_give_way_and_come_back_when_shown);
    test_run("a download under way is dropped when the connection goes", a_download_under_way_is_dropped_when_the_connection_goes);
    test_run("a reply's pictures follow it and open in the viewer", a_replys_pictures_follow_it_and_open_in_the_viewer);
}
