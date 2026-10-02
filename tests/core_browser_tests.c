// browser.c: the shared browser's event stream parser, base64, tabs, frame fitting, pointer mapping, keys and input queue.
#include "browser.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// MARK: - Server-sent events

typedef struct { char *events[16], *datas[16]; size_t lens[16]; int count; } Seen;
static void seen_emit(void *ctx, const char *event, const char *data, size_t len) {
    Seen *s = ctx;
    if (s->count >= 16) return;
    s->events[s->count] = xstrdup(event); s->datas[s->count] = xstrndup(data, len); s->lens[s->count] = len;
    s->count++;
}
static void seen_free(Seen *s) { for (int i = 0; i < s->count; i++) { free(s->events[i]); free(s->datas[i]); } memset(s, 0, sizeof *s); }
static void feed_all(const char *text, size_t piece, Seen *seen) {
    SseParser p; sse_init(&p);
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i += piece) sse_feed(&p, text + i, len - i < piece ? len - i : piece, seen_emit, seen);
    sse_free(&p);
}

static void test_events_carry_their_name_and_data(void) {
    Seen s = { 0 };
    feed_all("event: tabs\ndata: {\"tabs\":[]}\n\nevent: frame\ndata: {\"data\":\"QQ==\"}\n\n", 1000, &s);
    CHECK_INT(s.count, 2);
    CHECK_STR(s.events[0], "tabs"); CHECK_STR(s.datas[0], "{\"tabs\":[]}");
    CHECK_STR(s.events[1], "frame"); CHECK_STR(s.datas[1], "{\"data\":\"QQ==\"}");
    seen_free(&s);
}
static void test_events_survive_being_fed_a_byte_at_a_time_with_any_line_end(void) {
    const char *texts[] = { "event: a\r\ndata: 1\r\n\r\nevent: b\r\ndata: 2\r\n\r\n", "event: a\rdata: 1\r\revent: b\rdata: 2\r\r",
                            "event: a\ndata: 1\n\nevent: b\ndata: 2\n\n" };
    for (size_t t = 0; t < 3; t++) {
        for (size_t piece = 1; piece <= 3; piece++) {
            Seen s = { 0 };
            feed_all(texts[t], piece, &s);
            CHECK_INT(s.count, 2);
            CHECK_STR(s.events[0], "a"); CHECK_STR(s.datas[0], "1");
            CHECK_STR(s.events[1], "b"); CHECK_STR(s.datas[1], "2");
            seen_free(&s);
        }
    }
}
static void test_pings_and_unknown_fields_are_skipped_and_data_lines_join(void) {
    Seen s = { 0 };
    feed_all(": ping\n\nid: 4\nretry: 100\ndata:first\ndata: second\n\ndata\n\n", 7, &s);
    CHECK_INT(s.count, 2);
    CHECK_STR(s.events[0], "message"); CHECK_STR(s.datas[0], "first\nsecond");
    // `data` with no colon is an empty line of data.
    CHECK_STR(s.events[1], "message"); CHECK_STR(s.datas[1], "");
    seen_free(&s);
}
static void test_an_event_without_data_or_without_its_blank_line_is_not_emitted(void) {
    Seen s = { 0 };
    feed_all("event: closed\n\nevent: frame\ndata: {}", 4, &s);
    CHECK_INT(s.count, 0);
    seen_free(&s);
    // The name does not leak into the next event.
    feed_all("event: tabs\n\ndata: x\n\n", 100, &s);
    CHECK_INT(s.count, 1); CHECK_STR(s.events[0], "message");
    seen_free(&s);
}
static void test_a_long_frame_line_arrives_whole(void) {
    size_t n = 300000;
    char *text = xmalloc(n + 32);
    strcpy(text, "event: frame\ndata: ");
    size_t at = strlen(text);
    for (size_t i = 0; i < n; i++) text[at + i] = (char)('A' + i % 26);
    strcpy(text + at + n, "\n\n");
    Seen s = { 0 };
    feed_all(text, 16384, &s);
    CHECK_INT(s.count, 1);
    CHECK_INT(s.lens[0], n);
    CHECK(s.datas[0] && s.datas[0][0] == 'A' && s.datas[0][n - 1] == (char)('A' + (n - 1) % 26));
    seen_free(&s); free(text);
}

static void test_base64_decodes_with_and_without_padding(void) {
    size_t len;
    unsigned char *b = base64_decode("aGVsbG8=", 8, &len);
    CHECK(b && len == 5 && memcmp(b, "hello", 5) == 0); free(b);
    b = base64_decode("aGVsbG8", 7, &len);
    CHECK(b && len == 5 && memcmp(b, "hello", 5) == 0); free(b);
    b = base64_decode("/9j/", 4, &len);
    CHECK(b && len == 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF); free(b);
    b = base64_decode("", 0, &len);
    CHECK(b && len == 0); free(b);
    CHECK(base64_decode("aGV*bG8=", 8, &len) == NULL);
    CHECK(base64_decode("a", 1, &len) == NULL);
    CHECK(base64_decode(NULL, 0, &len) == NULL);
}

// MARK: - State

static void test_tabs_and_the_active_one_are_read_from_an_event(void) {
    BrowserState s = { 0 };
    Json *j = json_parsez("{\"tabs\":[{\"id\":\"t1\",\"url\":\"https://a.test/\",\"title\":\"A\"},{\"id\":\"\"},{\"id\":\"t2\",\"url\":\"about:blank\"}],\"active\":\"t2\"}");
    browser_state_read(&s, j);
    CHECK_INT(s.count, 2);
    CHECK_STR(s.tabs[0].id, "t1"); CHECK_STR(s.tabs[0].title, "A");
    CHECK_STR(s.tabs[1].id, "t2"); CHECK_STR(s.tabs[1].title, "");
    CHECK_STR(browser_active_tab(&s)->url, "about:blank");
    CHECK(!s.on && !s.running);
    json_free(j);
    // A record says whether it is on and up; one without tabs keeps them.
    j = json_parsez("{\"on\":true,\"running\":true,\"active\":null}");
    browser_state_read(&s, j);
    CHECK(s.on && s.running); CHECK_INT(s.count, 2); CHECK(s.active == NULL); CHECK(browser_active_tab(&s) == NULL);
    json_free(j);
    browser_state_free(&s);
    CHECK_INT(s.count, 0);
}
static void test_a_session_says_whether_its_browser_is_on_and_up(void) {
    bool running = true;
    Json *j = json_parsez("{\"browser\":null}");
    CHECK(!browser_session_on(j, &running)); CHECK(!running); json_free(j);
    j = json_parsez("{}");
    CHECK(!browser_session_on(j, &running)); json_free(j);
    j = json_parsez("{\"browser\":{\"running\":false}}");
    CHECK(browser_session_on(j, &running)); CHECK(!running); json_free(j);
    j = json_parsez("{\"browser\":{\"running\":true}}");
    CHECK(browser_session_on(j, &running)); CHECK(running); json_free(j);
    CHECK(!browser_session_on(NULL, NULL));
}

// MARK: - Drawing and pointing

static void test_a_frame_fits_its_area_without_stretching_or_growing(void) {
    BrowserRect r = browser_fit(1000, 500, 1280, 720);
    CHECK_INT(r.right - r.left, 888); CHECK_INT(r.bottom - r.top, 500); CHECK_INT(r.left, 56); CHECK_INT(r.top, 0);
    r = browser_fit(1000, 1000, 1280, 720);
    CHECK_INT(r.right - r.left, 1000); CHECK_INT(r.bottom - r.top, 562); CHECK_INT(r.top, 219);
    // A larger area shows it at its own size.
    r = browser_fit(3000, 2000, 1280, 720);
    CHECK_INT(r.right - r.left, 1280); CHECK_INT(r.bottom - r.top, 720); CHECK_INT(r.left, 860); CHECK_INT(r.top, 640);
    r = browser_fit(0, 500, 1280, 720);
    CHECK_INT(r.right, 0); CHECK_INT(r.bottom, 0);
    r = browser_fit(500, 500, 0, 720);
    CHECK_INT(r.right, 0);
}
static void test_a_view_point_maps_back_to_the_page(void) {
    BrowserRect drawn = { 100, 50, 740, 410 };   // 640 × 360 for a 1280 × 720 page
    double x, y;
    CHECK(browser_page_point(&drawn, 1280, 720, 100, 50, &x, &y));
    CHECK(x >= 0 && x < 2 && y >= 0 && y < 2);
    CHECK(browser_page_point(&drawn, 1280, 720, 420, 230, &x, &y));
    CHECK(x > 640 && x < 642 && y > 360 && y < 362);
    CHECK(browser_page_point(&drawn, 1280, 720, 739, 409, &x, &y));
    CHECK(x < 1280 && y < 720);
    CHECK(!browser_page_point(&drawn, 1280, 720, 99, 60, &x, &y));
    CHECK(!browser_page_point(&drawn, 1280, 720, 740, 60, &x, &y));
    CHECK(!browser_page_point(&drawn, 1280, 720, 200, 410, &x, &y));
    CHECK(!browser_page_point(&drawn, 0, 720, 200, 60, &x, &y));
}
static void test_keys_are_named_as_the_server_takes_them(void) {
    CHECK_STR(browser_key_name(VK_RETURN), "Enter"); CHECK_STR(browser_key_name(VK_BACK), "Backspace");
    CHECK_STR(browser_key_name(VK_PRIOR), "PageUp"); CHECK_STR(browser_key_name(VK_DOWN), "ArrowDown");
    CHECK_STR(browser_key_name('A'), "a"); CHECK_STR(browser_key_name('Z'), "z"); CHECK_STR(browser_key_name('7'), "7");
    CHECK(browser_key_name(VK_F5) == NULL); CHECK(browser_key_name(VK_SHIFT) == NULL);
}

// MARK: - Input

static Json *input(const char *json) { return json_parsez(json); }
static void test_typing_merges_moves_replace_each_other_and_scrolls_add_up(void) {
    BrowserInputs q = { 0 };
    browser_inputs_push(&q, input("{\"type\":\"type\",\"text\":\"he\"}"));
    browser_inputs_push(&q, input("{\"type\":\"type\",\"text\":\"llo\"}"));
    browser_inputs_push(&q, input("{\"type\":\"down\",\"x\":1,\"y\":1}"));
    browser_inputs_push(&q, input("{\"type\":\"move\",\"x\":2,\"y\":2}"));
    browser_inputs_push(&q, input("{\"type\":\"move\",\"x\":3,\"y\":3}"));
    browser_inputs_push(&q, input("{\"type\":\"up\",\"x\":3,\"y\":3}"));
    browser_inputs_push(&q, input("{\"type\":\"key\",\"key\":\"Enter\"}"));
    browser_inputs_push(&q, input("{\"type\":\"key\",\"key\":\"Enter\"}"));
    browser_inputs_push(&q, NULL);
    CHECK_INT(q.count, 6);
    Json *j = browser_inputs_pop(&q);
    CHECK_STR(json_str(json_get(j, "text")), "hello"); json_free(j);
    j = browser_inputs_pop(&q); CHECK_STR(json_str(json_get(j, "type")), "down"); json_free(j);
    j = browser_inputs_pop(&q); CHECK_INT(json_int_or(json_get(j, "x"), 0), 3); json_free(j);
    j = browser_inputs_pop(&q); CHECK_STR(json_str(json_get(j, "type")), "up"); json_free(j);
    j = browser_inputs_pop(&q); CHECK_STR(json_str(json_get(j, "key")), "Enter"); json_free(j);
    j = browser_inputs_pop(&q); CHECK_STR(json_str(json_get(j, "key")), "Enter"); json_free(j);
    CHECK(browser_inputs_pop(&q) == NULL);
    browser_inputs_push(&q, input("{\"type\":\"wheel\",\"x\":5,\"y\":5,\"deltaY\":100}"));
    browser_inputs_push(&q, input("{\"type\":\"wheel\",\"x\":6,\"y\":6,\"deltaY\":100,\"deltaX\":-20}"));
    CHECK_INT(q.count, 1);
    j = browser_inputs_pop(&q);
    CHECK_INT(json_int_or(json_get(j, "deltaY"), 0), 200); CHECK_INT(json_int_or(json_get(j, "deltaX"), 0), -20); CHECK_INT(json_int_or(json_get(j, "x"), 0), 6);
    json_free(j);
    browser_inputs_push(&q, input("{\"type\":\"type\",\"text\":\"x\"}"));
    browser_inputs_free(&q);
    CHECK_INT(q.count, 0);
}
static void test_typed_addresses_become_web_urls(void) {
    CHECK_OWNED_STR(browser_address("example.com"), "https://example.com");
    CHECK_OWNED_STR(browser_address("  https://Example.com/a?b=1  "), "https://Example.com/a?b=1");
    CHECK_OWNED_STR(browser_address("http://site.test"), "http://site.test");
    CHECK_OWNED_STR(browser_address("example.com:8443/x"), "https://example.com:8443/x");
    CHECK_OWNED_STR(browser_address("localhost:3000/login"), "http://localhost:3000/login");
    CHECK_OWNED_STR(browser_address("127.0.0.1"), "http://127.0.0.1");
    CHECK_OWNED_STR(browser_address("en.wikipedia.org/wiki/Special:Search"), "https://en.wikipedia.org/wiki/Special:Search");
    CHECK_OWNED_STR(browser_address("site.test?next=a:b#x:y"), "https://site.test?next=a:b#x:y");
    CHECK_OWNED_STR(browser_address("site.test/go?to=https://other.test"), "https://site.test/go?to=https://other.test");
    CHECK_OWNED_STR(browser_address("About:Blank"), "about:blank");
    CHECK(browser_address("") == NULL); CHECK(browser_address("   ") == NULL); CHECK(browser_address(NULL) == NULL);
    CHECK(browser_address("file:///C:/x") == NULL); CHECK(browser_address("chrome://settings") == NULL);
    CHECK(browser_address("javascript:alert(1)") == NULL); CHECK(browser_address("mailto:a@b.c") == NULL);
    CHECK(browser_address("two words") == NULL); CHECK(browser_address("https://") == NULL);
}

void browser_tests(void) {
    test_run("events carry their name and data", test_events_carry_their_name_and_data);
    test_run("events survive being fed a byte at a time with any line end", test_events_survive_being_fed_a_byte_at_a_time_with_any_line_end);
    test_run("pings and unknown fields are skipped and data lines join", test_pings_and_unknown_fields_are_skipped_and_data_lines_join);
    test_run("an event without data or without its blank line is not emitted", test_an_event_without_data_or_without_its_blank_line_is_not_emitted);
    test_run("a long frame line arrives whole", test_a_long_frame_line_arrives_whole);
    test_run("base64 decodes with and without padding", test_base64_decodes_with_and_without_padding);
    test_run("tabs and the active one are read from an event", test_tabs_and_the_active_one_are_read_from_an_event);
    test_run("a session says whether its browser is on and up", test_a_session_says_whether_its_browser_is_on_and_up);
    test_run("a frame fits its area without stretching or growing", test_a_frame_fits_its_area_without_stretching_or_growing);
    test_run("a view point maps back to the page", test_a_view_point_maps_back_to_the_page);
    test_run("keys are named as the server takes them", test_keys_are_named_as_the_server_takes_them);
    test_run("typing merges, moves replace each other and scrolls add up", test_typing_merges_moves_replace_each_other_and_scrolls_add_up);
    test_run("typed addresses become web URLs", test_typed_addresses_become_web_urls);
}
