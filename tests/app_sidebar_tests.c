// Swift #113 parity: exercise the real sidebar layouts and pane input/scrollbar handling.
#include "screen_fixture.h"
#include "screen_projects_internal.h"
#include "suites.h"
#include <windowsx.h>

static Route routes[] = { { "GET", "/projects", "read" } };
static wchar_t cache_directory[MAX_PATH];

static void draw(Pane *pane) {
    RedrawWindow(pane_hwnd(pane), NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}
static void resize(Pane *pane, int width, int height) {
    RECT bounds = { 0, 0, px(width), px(height) };
    pane_set_bounds(pane, &bounds); draw(pane);
}
static int action_item(Doc *doc, int action, intptr_t arg) {
    for (size_t i = 0; i < doc->count; i++)
        if (doc->items[i].action == action && doc->items[i].arg == arg) return (int)i;
    CHECK(false); return 0;
}
static POINT item_center(Pane *pane, int index) {
    RECT rc = pane_doc(pane)->items[index].rc, content = pane_content_rect(pane);
    POINT pt = { px(10) + (rc.left + rc.right) / 2, content.top + (rc.top + rc.bottom) / 2 - pane_scroll_y(pane) };
    return pt;
}
static void click(Pane *pane, POINT pt) {
    HWND hwnd = pane_hwnd(pane);
    SendMessageW(hwnd, WM_LBUTTONDOWN, 0, MAKELPARAM(pt.x, pt.y));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    draw(pane);
}
static void wheel(Pane *pane, int delta) {
    POINT pt = { px(40), px(100) }; ClientToScreen(pane_hwnd(pane), &pt);
    SendMessageW(pane_hwnd(pane), WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(pt.x, pt.y));
    draw(pane);
}
static void start(ScreenFixture *f) {
    screen_fixture_init(f, routes, sizeof routes / sizeof *routes);
    GetTempPathW(MAX_PATH, cache_directory); wcscat(cache_directory, L"briareus-sidebar-tests");
    g_store.cache = cache_new(cache_directory); cache_remove_all(g_store.cache);
    f->parent = CreateWindowExW(0, L"STATIC", L"Sidebar scroll fixture", WS_POPUP,
        0, 0, 1000, 2400, NULL, NULL, GetModuleHandleW(NULL), NULL);
    CHECK(f->parent != NULL); f->pane = pane_create(f->parent, true);
    ShowWindow(f->parent, SW_SHOWNA);
}
static void finish(ScreenFixture *f) {
    DiskCache *cache = g_store.cache;
    screen_fixture_clear(f);
    cache_remove_all(cache); cache_free(cache); RemoveDirectoryW(cache_directory);
}
static Screen *load_projects(ScreenFixture *f) {
    Str reply; str_init(&reply); str_appendz(&reply, "[");
    for (int i = 0; i < 30; i++)
        str_appendf(&reply, "%s{\"id\":%d,\"repo\":\"o/r%d\",\"name\":\"Project %d\"}", i ? "," : "", i + 1, i, i);
    str_appendz(&reply, "]"); f->read_response = reply.data;
    Screen *s = projects_screen_new(); pane_set_root(f->pane, s);
    s->vt->refresh(s); screen_fixture_pump(f, 1);
    f->read_response = NULL; str_free(&reply);
    return s;
}
static Screen *new_sessions(void) {
    Project project; memset(&project, 0, sizeof project); project.repo = "o/r";
    SessionsScreen *s = (SessionsScreen *)sessions_screen_new(&project);
    Json *list = json_array();
    for (int i = 0; i < 30; i++) {
        Json *row = json_object(); char id[16]; snprintf(id, sizeof id, "s%d", i);
        json_set_str(row, "id", id); json_set_str(row, "repo", "o/r"); json_set_str(row, "title", "Conversation");
        json_set_str(row, "status", "idle");
        json_array_push(list, row);
    }
    CHECK(sessions_parse(list, &s->sessions, &s->count)); CHECK_INT(s->count, 30); json_free(list);
    s->loaded = true; s->select_mode = true;
    return &s->base;
}
static void check_strip(Pane *pane) {
    Doc *doc = pane_doc(pane);
    CHECK(doc->pin_last > 0); CHECK_INT(doc->pin_bottom, px(56));
    CHECK_INT(doc->full_width, pane_content_width(pane));
    int actions[] = { ACT_NEW, ACT_WHATSAPP, ACT_STATUS, ACT_DASHBOARD, ACT_FINDINGS };
    for (size_t i = 0; i < sizeof actions / sizeof *actions; i++) {
        int index = action_item(doc, actions[i], 0); Item *it = &doc->items[index];
        CHECK(index < doc->pin_last);
        CHECK_INT(it->rc.top - pane_scroll_y(pane), px(10));
        CHECK_INT(it->rc.bottom - pane_scroll_y(pane), px(42));
        CHECK_INT(doc_hit(doc, (it->rc.left + it->rc.right) / 2, pane_scroll_y(pane) + px(20)), index);
    }
    CHECK_INT(doc->items[action_item(doc, ACT_FINDINGS, 0)].rc.right, pane_content_width(pane));
    CHECK_INT(pane_scroll_x(pane), 0);
}
static void check_scroll_inputs(Pane *pane, int row_action) {
    resize(pane, 320, 300); check_strip(pane);
    Doc *doc = pane_doc(pane); CHECK_INT(doc->width, doc->full_width - px(10));
    int row = action_item(doc, row_action, 0);
    CHECK_INT(doc->items[row].rc.right, doc->width);
    int row_y = doc->items[row].rc.top;
    RECT page_view = pane_content_rect(pane);
    int previous_bottom = page_view.bottom - page_view.top;
    SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_NEXT, 0); draw(pane);
    CHECK(pane_scroll_y(pane) > 0); check_strip(pane);
    CHECK_INT(previous_bottom - (pane_scroll_y(pane) + doc->pin_bottom), px(40));
    int first_page = pane_scroll_y(pane);
    SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_NEXT, 0); draw(pane);
    CHECK_INT(pane_scroll_y(pane), first_page * 2);
    SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_PRIOR, 0); draw(pane);
    CHECK_INT(pane_scroll_y(pane), first_page);
    CHECK_INT(doc->items[row].rc.top, row_y);
    // Scrolled rows under the fixed band must not intercept its controls or its padding.
    CHECK_INT(doc_hit(doc, px(1), pane_scroll_y(pane) + px(50)), -1);
    int before = pane_scroll_y(pane); wheel(pane, -WHEEL_DELTA);
    CHECK(pane_scroll_y(pane) > before); check_strip(pane);
    // A scrollbar drag starts below the strip, reaches the end, and never crosses the footer.
    pane_scroll_to_top(pane); draw(pane);
    RECT view = pane_content_rect(pane);
    int x = view.right - px(5), y = view.top + doc->pin_bottom + 1;
    SendMessageW(pane_hwnd(pane), WM_LBUTTONDOWN, 0, MAKELPARAM(x, y));
    SendMessageW(pane_hwnd(pane), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, view.bottom - 1));
    SendMessageW(pane_hwnd(pane), WM_LBUTTONUP, 0, MAKELPARAM(x, view.bottom - 1));
    draw(pane); CHECK(pane_at_bottom(pane)); check_strip(pane);
    CHECK_INT(pane_scroll_y(pane), doc_height(doc) + px(12) - (view.bottom - view.top));
    pane_scroll_to_top(pane); draw(pane);
    CHECK_INT(doc_hit(doc, px(20), doc->items[row].rc.top + 1), row);
}
static void check_short_paging(Pane *pane) {
    pane_scroll_to_top(pane); resize(pane, 320, 300);
    RECT view = pane_content_rect(pane);
    int chrome = px(300) - (view.bottom - view.top);
    RECT bounds = { 0, 0, px(320), chrome + pane_doc(pane)->pin_bottom + px(20) };
    pane_set_bounds(pane, &bounds); draw(pane); view = pane_content_rect(pane);
    int visible = view.bottom - view.top - pane_doc(pane)->pin_bottom;
    CHECK(visible > 0 && visible < px(40));
    SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_NEXT, 0); draw(pane);
    CHECK_INT(pane_scroll_y(pane), 1);
    SendMessageW(pane_hwnd(pane), WM_KEYDOWN, VK_PRIOR, 0); draw(pane);
    CHECK_INT(pane_scroll_y(pane), 0);
}
static void check_resizes(Pane *pane) {
    int widths[] = { 240, 480 };
    for (size_t i = 0; i < sizeof widths / sizeof *widths; i++) {
        resize(pane, widths[i], 240); wheel(pane, -WHEEL_DELTA); check_strip(pane);
        CHECK(pane_scroll_y(pane) > 0);
        resize(pane, widths[i], 2200); check_strip(pane);
        CHECK_INT(pane_scroll_y(pane), 0);
        CHECK_INT(pane_doc(pane)->width, pane_content_width(pane));
        wheel(pane, -WHEEL_DELTA); CHECK_INT(pane_scroll_y(pane), 0);
    }
}
static void test_project_sidebar(void) {
    ScreenFixture f; start(&f); Screen *s = load_projects(&f);
    check_scroll_inputs(f.pane, ACT_OPEN_PROJECT); check_short_paging(f.pane); check_resizes(f.pane);
    resize(f.pane, 320, 300); wheel(f.pane, -WHEEL_DELTA);
    Doc *doc = pane_doc(f.pane); int row = action_item(doc, ACT_OPEN_PROJECT, 2);
    click(f.pane, item_center(f.pane, row));
    CHECK(pane_top(f.pane) != s); CHECK_INT(pane_scroll_y(f.pane), 0); check_strip(f.pane);
    click(f.pane, item_center(f.pane, action_item(pane_doc(f.pane), ACT_BACK, 0)));
    CHECK(pane_top(f.pane) == s); CHECK_INT(pane_scroll_y(f.pane), 0); check_strip(f.pane);
    finish(&f);
}
static void test_session_sidebar(void) {
    ScreenFixture f; start(&f); Screen *s = new_sessions(); pane_set_root(f.pane, s);
    check_scroll_inputs(f.pane, ACT_PICK); check_short_paging(f.pane); check_resizes(f.pane);
    theme_set_dpi(144); pane_relayout(f.pane);
    resize(f.pane, 320, 300); wheel(f.pane, -WHEEL_DELTA); check_strip(f.pane);
    Doc *doc = pane_doc(f.pane); int row = action_item(doc, ACT_PICK, 0);
    click(f.pane, item_center(f.pane, row));
    CHECK_INT(((SessionsScreen *)s)->picked_count, 1);
    CHECK_STR(((SessionsScreen *)s)->picked[0], "s0");
    SendMessageW(pane_hwnd(f.pane), WM_KEYDOWN, VK_ESCAPE, 0); draw(f.pane);
    CHECK(!((SessionsScreen *)s)->select_mode); check_strip(f.pane);
    // Refreshing selected detail/filter state preserves the pin and clamps a now-empty list.
    pane_set_selected_id(f.pane, "findings"); draw(f.pane); check_strip(f.pane);
    sessions_free(((SessionsScreen *)s)->sessions, ((SessionsScreen *)s)->count);
    ((SessionsScreen *)s)->sessions = NULL; ((SessionsScreen *)s)->count = 0;
    pane_relayout(f.pane); draw(f.pane); check_strip(f.pane); CHECK_INT(pane_scroll_y(f.pane), 0);
    finish(&f); theme_set_dpi(96);
}
void app_sidebar_tests(void) {
    screen_fixture_prepare();
    test_run("project sidebar pins actions above scrolling rows and survives navigation and resize (Swift #113)", test_project_sidebar);
    test_run("session sidebar pins actions through wheel, keyboard, drag, DPI and selection changes (Swift #113)", test_session_sidebar);
}
