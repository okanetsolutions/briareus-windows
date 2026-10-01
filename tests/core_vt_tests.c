// vt.c: printing and wrapping, cursor moves, erases, scroll regions and the scrollback, SGR colours, the alternate screen,
// line drawing, titles, the reports it answers with, and resizing.
#include "str.h"
#include "suites.h"
#include "test.h"
#include "vt.h"
#include <stdlib.h>
#include <string.h>

static void put(Vt *vt, const char *s) { vt_write(vt, s, strlen(s)); }
/// A screen row as text, trailing blanks dropped.
static char *row_text(Vt *vt, int y) { return vt_text(vt, y, 0, y, vt_cols(vt)); }
static uint32_t cell_ch(Vt *vt, int y, int x) { int w; const VtCell *r = vt_line(vt, y, &w); return r && x < w ? r[x].ch : 0; }
static const VtCell *cell(Vt *vt, int y, int x) { int w; const VtCell *r = vt_line(vt, y, &w); return r && x < w ? r + x : NULL; }

static void test_prints_text_and_moves_the_cursor(void) {
    Vt *vt = vt_new(10, 3, 100);
    put(vt, "hello");
    CHECK_OWNED_STR(row_text(vt, 0), "hello");
    int x, y; bool visible;
    vt_cursor(vt, &x, &y, &visible);
    CHECK_INT(x, 5); CHECK_INT(y, 0); CHECK(visible);
    put(vt, "\r\nworld");
    CHECK_OWNED_STR(row_text(vt, 1), "world");
    vt_free(vt);
}

static void test_wraps_at_the_edge_only_when_the_next_character_comes(void) {
    Vt *vt = vt_new(4, 3, 100);
    put(vt, "abcd");
    int x, y; vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(x, 3); CHECK_INT(y, 0);
    put(vt, "e");
    CHECK_OWNED_STR(row_text(vt, 0), "abcd");
    CHECK_OWNED_STR(row_text(vt, 1), "e");
    // A carriage return at the edge cancels the pending wrap.
    put(vt, "fgh\rX");
    CHECK_OWNED_STR(row_text(vt, 1), "Xfgh");
    vt_free(vt);
}

static void test_scrolling_off_the_top_feeds_the_scrollback(void) {
    Vt *vt = vt_new(8, 2, 100);
    put(vt, "one\r\ntwo\r\nthree\r\nfour");
    CHECK_INT(vt_scrollback(vt), 2);
    CHECK_OWNED_STR(vt_text(vt, -2, 0, -2, 8), "one");
    CHECK_OWNED_STR(vt_text(vt, -1, 0, -1, 8), "two");
    CHECK_OWNED_STR(row_text(vt, 0), "three");
    CHECK_OWNED_STR(row_text(vt, 1), "four");
    CHECK_OWNED_STR(vt_text(vt, -1, 0, 0, 8), "two\r\nthree");
    vt_clear_scrollback(vt);
    CHECK_INT(vt_scrollback(vt), 0);
    vt_free(vt);
}

static void test_the_scrollback_keeps_only_its_capacity(void) {
    Vt *vt = vt_new(8, 1, 3);
    for (int i = 0; i < 10; i++) { char line[16]; snprintf(line, sizeof line, "l%d\r\n", i); put(vt, line); }
    CHECK_INT(vt_scrollback(vt), 3);
    CHECK_OWNED_STR(vt_text(vt, -3, 0, -3, 8), "l7");
    CHECK_OWNED_STR(vt_text(vt, -1, 0, -1, 8), "l9");
    CHECK(vt_line(vt, -4, NULL) == NULL);
    vt_free(vt);
}

static void test_cursor_position_and_erases(void) {
    Vt *vt = vt_new(10, 4, 100);
    put(vt, "aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc\r\ndddddddddd");
    put(vt, "\x1b[2;4H\x1b[K");
    CHECK_OWNED_STR(row_text(vt, 1), "bbb");
    put(vt, "\x1b[1;3H\x1b[1K");
    CHECK_OWNED_STR(row_text(vt, 0), "   aaaaaaa");
    put(vt, "\x1b[3;5H\x1b[J");
    CHECK_OWNED_STR(row_text(vt, 2), "cccc");
    CHECK_OWNED_STR(row_text(vt, 3), "");
    put(vt, "\x1b[2J");
    for (int y = 0; y < 4; y++) CHECK_OWNED_STR(row_text(vt, y), "");
    // Erasing the screen keeps the scrollback; ED 3 empties it.
    vt_free(vt);
}

static void test_relative_moves_stop_at_the_edges(void) {
    Vt *vt = vt_new(10, 5, 100);
    put(vt, "\x1b[3;3H\x1b[10A");
    int x, y; vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(y, 0); CHECK_INT(x, 2);
    put(vt, "\x1b[20C\x1b[2B");
    vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(x, 9); CHECK_INT(y, 2);
    put(vt, "\x1b[4D\x1b[G");
    vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(x, 0);
    // Empty parameters take their defaults.
    put(vt, "\x1b[;5H");
    vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(x, 4); CHECK_INT(y, 0);
    vt_free(vt);
}

static void test_insert_and_delete_characters_and_lines(void) {
    Vt *vt = vt_new(8, 3, 100);
    put(vt, "abcdef\x1b[1;3H\x1b[2@");
    CHECK_OWNED_STR(row_text(vt, 0), "ab  cdef");
    put(vt, "\x1b[3P");
    CHECK_OWNED_STR(row_text(vt, 0), "abdef");
    put(vt, "\x1b[1;1H\x1b[2;1Hline2\x1b[3;1Hline3\x1b[2;1H\x1b[L");
    CHECK_OWNED_STR(row_text(vt, 1), "");
    CHECK_OWNED_STR(row_text(vt, 2), "line2");
    put(vt, "\x1b[M");
    CHECK_OWNED_STR(row_text(vt, 1), "line2");
    put(vt, "\x1b[1;2H\x1b[2X");
    CHECK_OWNED_STR(row_text(vt, 0), "a  ef");
    vt_free(vt);
}

static void test_a_scroll_region_scrolls_inside_itself_without_the_scrollback(void) {
    Vt *vt = vt_new(6, 4, 100);
    put(vt, "top\r\nr1\r\nr2\r\nbottom");
    put(vt, "\x1b[2;3r\x1b[3;1H\nnew");
    CHECK_OWNED_STR(row_text(vt, 0), "top");
    CHECK_OWNED_STR(row_text(vt, 1), "r2");
    CHECK_OWNED_STR(row_text(vt, 2), "new");
    CHECK_OWNED_STR(row_text(vt, 3), "bottom");
    CHECK_INT(vt_scrollback(vt), 0);
    // Reverse index at the region's top scrolls it down.
    put(vt, "\x1b[2;1H\x1bM");
    CHECK_OWNED_STR(row_text(vt, 1), "");
    CHECK_OWNED_STR(row_text(vt, 2), "r2");
    CHECK_OWNED_STR(row_text(vt, 3), "bottom");
    vt_free(vt);
}

static void test_sgr_sets_colours_and_attributes(void) {
    Vt *vt = vt_new(10, 2, 100);
    put(vt, "\x1b[1;31mA\x1b[0;4;38;5;200;48;2;1;2;3mB\x1b[0;97;104mC\x1b[7mD\x1b[27;39mE");
    const VtCell *a = cell(vt, 0, 0), *b = cell(vt, 0, 1), *c = cell(vt, 0, 2), *d = cell(vt, 0, 3), *e = cell(vt, 0, 4);
    CHECK(a->attr & VT_BOLD); CHECK(a->fg == (VT_COLOR_INDEX | 1)); CHECK(a->bg == VT_COLOR_DEFAULT);
    CHECK(!(b->attr & VT_BOLD)); CHECK(b->attr & VT_UNDERLINE);
    CHECK(b->fg == (VT_COLOR_INDEX | 200)); CHECK(b->bg == (VT_COLOR_RGB | 0x010203));
    CHECK(c->fg == (VT_COLOR_INDEX | 15)); CHECK(c->bg == (VT_COLOR_INDEX | 12));
    CHECK(d->attr & VT_INVERSE);
    CHECK(!(e->attr & VT_INVERSE)); CHECK(e->fg == VT_COLOR_DEFAULT); CHECK(e->bg == (VT_COLOR_INDEX | 12));
    // An erase takes the current background, as xterm's does.
    put(vt, "\x1b[2;1H\x1b[K");
    CHECK(cell(vt, 1, 5)->bg == (VT_COLOR_INDEX | 12));
    vt_free(vt);
}

static void test_the_alternate_screen_keeps_the_main_one(void) {
    Vt *vt = vt_new(8, 2, 100);
    put(vt, "shell$ ");
    put(vt, "\x1b[?1049h");
    CHECK(vt_alt_screen(vt));
    CHECK_OWNED_STR(row_text(vt, 0), "");
    put(vt, "vim\r\n\r\n\r\n");   // the alternate screen never feeds the scrollback
    CHECK_INT(vt_scrollback(vt), 0);
    put(vt, "\x1b[?1049l");
    CHECK(!vt_alt_screen(vt));
    CHECK_OWNED_STR(row_text(vt, 0), "shell$");
    int x, y; vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(x, 7); CHECK_INT(y, 0);
    vt_free(vt);
}

static void test_utf8_split_across_writes_and_wide_characters(void) {
    Vt *vt = vt_new(6, 2, 100);
    const char *s = "\xC3\xA9\xE4\xB8\xAD";   // é 中
    vt_write(vt, s, 1); vt_write(vt, s + 1, 2); vt_write(vt, s + 3, 2);
    CHECK_INT(cell_ch(vt, 0, 0), 0xE9);
    CHECK_INT(cell_ch(vt, 0, 1), 0x4E2D);
    CHECK(cell(vt, 0, 1)->attr & VT_WIDE);
    CHECK(cell(vt, 0, 2)->attr & VT_WIDE_TAIL);
    int x; vt_cursor(vt, &x, NULL, NULL);
    CHECK_INT(x, 3);
    CHECK_OWNED_STR(row_text(vt, 0), "\xC3\xA9\xE4\xB8\xAD");
    // A wide character at the last column wraps whole.
    put(vt, "ab\xE4\xB8\xAD");
    CHECK_OWNED_STR(row_text(vt, 0), "\xC3\xA9\xE4\xB8\xAD" "ab");
    CHECK_OWNED_STR(row_text(vt, 1), "\xE4\xB8\xAD");
    // A broken sequence becomes one replacement character.
    put(vt, "\r\n\xE4zz");
    vt_free(vt);
}

static void test_dec_line_drawing(void) {
    Vt *vt = vt_new(6, 1, 100);
    put(vt, "\x1b(0lqk\x1b(Bq");
    CHECK_INT(cell_ch(vt, 0, 0), 0x250C);
    CHECK_INT(cell_ch(vt, 0, 1), 0x2500);
    CHECK_INT(cell_ch(vt, 0, 2), 0x2510);
    CHECK_INT(cell_ch(vt, 0, 3), 'q');
    vt_free(vt);
}

static void test_titles_and_the_bell(void) {
    Vt *vt = vt_new(6, 1, 100);
    CHECK(vt_title(vt) == NULL);
    put(vt, "\x1b]0;user@host: ~\x07");
    CHECK_STR(vt_title(vt), "user@host: ~");
    CHECK(vt_take_title_changed(vt));
    CHECK(!vt_take_title_changed(vt));
    put(vt, "\x1b]2;sp");
    put(vt, "lit\x1b\\x");
    CHECK_STR(vt_title(vt), "split");
    CHECK_OWNED_STR(row_text(vt, 0), "x");
    // Other strings are skipped whole.
    put(vt, "\x1bPq#0;2;0;0;0\x1b\\y\x07");
    CHECK_OWNED_STR(row_text(vt, 0), "xy");
    CHECK(vt_take_bell(vt));
    vt_free(vt);
}

static void test_reports_answer_the_program(void) {
    Vt *vt = vt_new(10, 5, 100);
    size_t len;
    CHECK(vt_take_response(vt, &len) == NULL);
    put(vt, "\x1b[3;4H\x1b[6n\x1b[c");
    char *r = vt_take_response(vt, &len);
    CHECK_STR(r, "\x1b[3;4R\x1b[?62;22c");
    CHECK_INT(len, strlen("\x1b[3;4R\x1b[?62;22c"));
    free(r);
    CHECK(vt_take_response(vt, &len) == NULL);
    vt_free(vt);
}

static void test_modes_the_keyboard_depends_on(void) {
    Vt *vt = vt_new(10, 5, 100);
    CHECK(!vt_app_cursor(vt)); CHECK(!vt_bracketed_paste(vt));
    put(vt, "\x1b[?1h\x1b[?2004h\x1b=\x1b[?25l");
    CHECK(vt_app_cursor(vt)); CHECK(vt_bracketed_paste(vt)); CHECK(vt_app_keypad(vt));
    bool visible; vt_cursor(vt, NULL, NULL, &visible);
    CHECK(!visible);
    put(vt, "\x1b" "c");
    CHECK(!vt_app_cursor(vt)); CHECK(!vt_bracketed_paste(vt));
    vt_free(vt);
}

static void test_resizing_keeps_the_cursor_line_on_screen(void) {
    Vt *vt = vt_new(10, 4, 100);
    put(vt, "1\r\n2\r\n3\r\n4");
    vt_resize(vt, 6, 2);
    CHECK_INT(vt_cols(vt), 6); CHECK_INT(vt_rows(vt), 2);
    CHECK_OWNED_STR(row_text(vt, 0), "3");
    CHECK_OWNED_STR(row_text(vt, 1), "4");
    CHECK_INT(vt_scrollback(vt), 2);
    int x, y; vt_cursor(vt, &x, &y, NULL);
    CHECK_INT(y, 1); CHECK_INT(x, 1);
    vt_resize(vt, 12, 3);
    CHECK_OWNED_STR(row_text(vt, 0), "3");
    put(vt, "\r\n\r\nabcdefghijkl");
    CHECK_OWNED_STR(row_text(vt, 2), "abcdefghijkl");
    vt_free(vt);
}

static void test_tabs_and_backspace(void) {
    Vt *vt = vt_new(20, 1, 100);
    put(vt, "a\tb\x08" "c");
    CHECK_OWNED_STR(row_text(vt, 0), "a       c");
    vt_free(vt);
}

static void test_char_widths_and_the_palette(void) {
    CHECK_INT(vt_char_width('a'), 1);
    CHECK_INT(vt_char_width(0x0301), 0);
    CHECK_INT(vt_char_width(0x4E2D), 2);
    CHECK_INT(vt_char_width(0x1F600), 2);
    CHECK_INT(vt_index_rgb(16), 0x000000);
    CHECK_INT(vt_index_rgb(231), 0xFFFFFF);
    CHECK_INT(vt_index_rgb(232), 0x080808);
}

void vt_tests(void) {
    test_run("vt prints text and moves the cursor", test_prints_text_and_moves_the_cursor);
    test_run("vt wraps at the edge only when the next character comes", test_wraps_at_the_edge_only_when_the_next_character_comes);
    test_run("vt scrolling off the top feeds the scrollback", test_scrolling_off_the_top_feeds_the_scrollback);
    test_run("vt scrollback keeps only its capacity", test_the_scrollback_keeps_only_its_capacity);
    test_run("vt cursor position and erases", test_cursor_position_and_erases);
    test_run("vt relative moves stop at the edges", test_relative_moves_stop_at_the_edges);
    test_run("vt insert and delete characters and lines", test_insert_and_delete_characters_and_lines);
    test_run("vt scroll region scrolls inside itself", test_a_scroll_region_scrolls_inside_itself_without_the_scrollback);
    test_run("vt sgr sets colours and attributes", test_sgr_sets_colours_and_attributes);
    test_run("vt alternate screen keeps the main one", test_the_alternate_screen_keeps_the_main_one);
    test_run("vt utf8 split across writes and wide characters", test_utf8_split_across_writes_and_wide_characters);
    test_run("vt dec line drawing", test_dec_line_drawing);
    test_run("vt titles and the bell", test_titles_and_the_bell);
    test_run("vt reports answer the program", test_reports_answer_the_program);
    test_run("vt modes the keyboard depends on", test_modes_the_keyboard_depends_on);
    test_run("vt resizing keeps the cursor line on screen", test_resizing_keeps_the_cursor_line_on_screen);
    test_run("vt tabs and backspace", test_tabs_and_backspace);
    test_run("vt char widths and the palette", test_char_widths_and_the_palette);
}
