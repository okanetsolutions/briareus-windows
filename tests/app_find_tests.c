// Search the rendered document: markup is invisible, Unicode offsets use UTF-16, and layouts keep the current hit.
#include "doc.h"
#include "suites.h"
#include "test.h"
#include "theme.h"

static void begin_doc(Doc *doc, int width) {
    if (!theme.canvas) theme_init();
    doc_init(doc); doc_begin(doc, NULL, width);
}
static void test_find_rendered_text(void) {
    Doc doc; begin_doc(&doc, 500);
    doc_rich(&doc, 0, 500, "Find **API** and `api`, then [Api](https://example.com).", FONT_BODY, theme.ink);
    doc_end(&doc);
    doc_search(&doc, L"api");
    CHECK_INT(doc.match_count, 3);
    CHECK_INT(doc.matches[0].offset, 5);
    CHECK_INT(doc.matches[1].offset, 13);
    doc_search_step(&doc, true); CHECK_INT(doc.match_current, 2);
    doc_search_step(&doc, false); CHECK_INT(doc.match_current, 0);
    doc_search_step(&doc, false); CHECK_INT(doc.match_current, 1);
    // A query can cross bold/code/link boundaries in one paragraph.
    doc_search(&doc, L"API and api"); CHECK_INT(doc.match_count, 1);
    doc_search(&doc, L"https://example.com"); CHECK_INT(doc.match_count, 0);
    doc_search_step(&doc, true); CHECK_INT(doc.match_current, 0);
    doc_free(&doc);
}
static void test_find_unicode_and_wrapping(void) {
    Doc doc; begin_doc(&doc, 90);
    doc_rich(&doc, 0, 90, "\xF0\x9F\x98\x80 \xC3\x84pfel one two three four five six seven eight nine ten target", FONT_BODY, theme.ink);
    doc_end(&doc);
    doc_search(&doc, L"\x00E4pfel"); CHECK_INT(doc.match_count, 1);
    CHECK_INT(doc.matches[0].offset, 3); // The emoji occupies two UTF-16 units.
    doc_search(&doc, L"target"); CHECK_INT(doc.match_count, 1);
    RECT hit; CHECK(doc_search_rect(&doc, &hit));
    CHECK(hit.top > doc.items[0].rc.top); CHECK(hit.bottom <= doc.items[0].rc.bottom);
    doc_search(&doc, L""); CHECK_INT(doc.match_count, 0); CHECK(!doc_search_rect(&doc, &hit));
    CHECK_INT(doc.items[0].match_count, 0);
    doc_free(&doc);
}
static void test_find_survives_refresh(void) {
    Doc doc; begin_doc(&doc, 500);
    doc_rich(&doc, 0, 500, "first needle, second needle", FONT_BODY, theme.ink);
    doc_end(&doc); doc_search(&doc, L"needle"); doc_search_step(&doc, false);
    doc_begin(&doc, NULL, 200);
    doc_rich(&doc, 0, 200, "first needle, second needle, third needle", FONT_BODY, theme.ink);
    doc_end(&doc);
    CHECK_INT(doc.match_count, 3); CHECK_INT(doc.match_current, 1);
    doc_begin(&doc, NULL, 200);
    doc_rich(&doc, 0, 200, "no results remain", FONT_BODY, theme.ink);
    doc_end(&doc); CHECK_INT(doc.match_count, 0); CHECK_INT(doc.match_current, 0);
    doc_begin(&doc, NULL, 200);
    doc_rich(&doc, 0, 200, "needle returns", FONT_BODY, theme.ink);
    doc_end(&doc); CHECK_INT(doc.match_count, 1);
    doc_free(&doc);
}
static void test_find_code_and_selection(void) {
    Doc doc; begin_doc(&doc, 500);
    doc_markdown(&doc, 0, 500, "User asks for TOKEN.\n\n```\nTOKEN=secret\n```\n\nAssistant says token.", FONT_BODY);
    doc_end(&doc);
    doc_select_all(&doc);
    DocPos anchor = doc.sel_anchor, focus = doc.sel_focus;
    doc_search(&doc, L"token"); CHECK_INT(doc.match_count, 3);
    CHECK(doc.matches[0].item < doc.matches[1].item);
    CHECK(doc.matches[1].item < doc.matches[2].item);
    CHECK_INT(doc.sel_anchor.item, anchor.item); CHECK_INT(doc.sel_anchor.offset, anchor.offset);
    CHECK_INT(doc.sel_focus.item, focus.item); CHECK_INT(doc.sel_focus.offset, focus.offset);
    doc_search(&doc, NULL); CHECK_INT(doc.match_count, 0); CHECK(doc_has_selection(&doc));
    doc_free(&doc);
}
void app_find_tests(void) {
    test_run("find rendered text and wrap navigation", test_find_rendered_text);
    test_run("find Unicode and wrapped match coordinates", test_find_unicode_and_wrapping);
    test_run("find current match survives transcript refresh", test_find_survives_refresh);
    test_run("find code without changing text selection", test_find_code_and_selection);
}
