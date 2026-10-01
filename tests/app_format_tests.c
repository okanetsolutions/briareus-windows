// theme.c's formatting, colour and scaling helpers, and attach.c's file sizes.
#include "attach.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

static void ensure_theme(void) { if (!theme.canvas) theme_init(); }

static COLORREF hex(unsigned v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); }

// The local wall clock as the C runtime sees it, in the locale's words, to compare with the app's.
static SYSTEMTIME local_systemtime(time_t t) {
    struct tm tm = *localtime(&t);
    SYSTEMTIME st; memset(&st, 0, sizeof st);
    st.wYear = (WORD)(tm.tm_year + 1900); st.wMonth = (WORD)(tm.tm_mon + 1); st.wDay = (WORD)tm.tm_mday; st.wDayOfWeek = (WORD)tm.tm_wday;
    st.wHour = (WORD)tm.tm_hour; st.wMinute = (WORD)tm.tm_min; st.wSecond = (WORD)tm.tm_sec;
    return st;
}
static char *locale_clock(time_t t) {
    SYSTEMTIME st = local_systemtime(t);
    wchar_t buf[64];
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, buf, 64);
    return wide_to_utf8(buf);
}
static char *locale_date(time_t t, const wchar_t *picture) {
    SYSTEMTIME st = local_systemtime(t);
    wchar_t buf[64];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, picture, buf, 64, NULL);
    return wide_to_utf8(buf);
}

// MARK: - Times

static void test_event_time_today_is_the_clock_alone(void) {
    time_t now = time(NULL);
    char *expected = locale_clock(now);
    CHECK_OWNED_STR(format_event_time(now), expected);
    free(expected);
}

static void test_event_time_on_another_day_names_the_day(void) {
    time_t now = time(NULL);
    time_t when[2] = { now - 3 * 86400, now + 3 * 86400 };
    for (int i = 0; i < 2; i++) {
        char *day = locale_date(when[i], L"MMM d");
        char *got = format_event_time(when[i]);
        char *prefix = xstrfmt("%s, ", day);
        CHECK(str_has_prefix(got, prefix));
        char *clock = locale_clock(when[i]);
        char *expected = xstrfmt("%s, %s", day, clock);
        CHECK_STR(got, expected);
        free(expected); free(clock); free(prefix); free(got); free(day);
    }
}

static void test_event_times_take_the_offset_of_their_own_date(void) {
    // Noon UTC on 1 January and 1 July 2026: one in standard time and one in daylight time wherever the zone has both.
    time_t when[2] = { 1767268800, 1782907200 };
    for (int i = 0; i < 2; i++) {
        char *day = locale_date(when[i], L"MMM d"), *clock = locale_clock(when[i]);
        char *expected = xstrfmt("%s, %s", day, clock);
        CHECK_OWNED_STR(format_event_time(when[i]), expected);
        free(expected); free(clock); free(day);
        char *date = locale_date(when[i], L"MMM d, yyyy");
        CHECK_OWNED_STR(format_date_abbrev(when[i]), date);
        free(date);
    }
}

static void test_date_abbrev_is_month_day_and_year(void) {
    time_t now = time(NULL);
    char *expected = locale_date(now, L"MMM d, yyyy");
    char *got = format_date_abbrev(now);
    CHECK_STR(got, expected);
    struct tm tm = *localtime(&now);
    char *year = xstrfmt("%d", tm.tm_year + 1900);
    CHECK(strstr(got, year) != NULL);
    free(year); free(got); free(expected);
}

static void test_relative_time_steps_from_minutes_to_years(void) {
    time_t now = time(NULL);
    // Each case sits at a bucket's lower edge or inside it, so a second passing during the test changes nothing.
    CHECK_OWNED_STR(format_relative(now), "now");
    CHECK_OWNED_STR(format_relative(now - 30), "now");
    CHECK_OWNED_STR(format_relative(now - 60), "1m ago");
    CHECK_OWNED_STR(format_relative(now - 59 * 60 - 20), "59m ago");
    CHECK_OWNED_STR(format_relative(now - 3600), "1h ago");
    CHECK_OWNED_STR(format_relative(now - 23 * 3600 - 60), "23h ago");
    CHECK_OWNED_STR(format_relative(now - 86400), "1d ago");
    CHECK_OWNED_STR(format_relative(now - 6 * 86400 - 60), "6d ago");
    CHECK_OWNED_STR(format_relative(now - 7 * 86400), "1w ago");
    CHECK_OWNED_STR(format_relative(now - 29 * 86400), "4w ago");
    CHECK_OWNED_STR(format_relative(now - 30 * 86400), "1mo ago");
    CHECK_OWNED_STR(format_relative(now - 364 * 86400), "12mo ago");
    CHECK_OWNED_STR(format_relative(now - 365 * 86400), "1y ago");
    CHECK_OWNED_STR(format_relative(now - 3 * 365 * 86400 - 60), "3y ago");
}

static void test_relative_time_in_the_future_has_no_ago(void) {
    time_t now = time(NULL);
    CHECK_OWNED_STR(format_relative(now + 30), "now");
    CHECK_OWNED_STR(format_relative(now + 150), "2m");
    CHECK_OWNED_STR(format_relative(now + 2 * 3600 + 30), "2h");
    CHECK_OWNED_STR(format_relative(now + 3 * 86400 + 30), "3d");
}

static void test_duration_shows_the_two_largest_units(void) {
    CHECK_OWNED_STR(format_duration_ms(0), "0s");
    CHECK_OWNED_STR(format_duration_ms(999), "0s");
    CHECK_OWNED_STR(format_duration_ms(1000), "1s");
    CHECK_OWNED_STR(format_duration_ms(59999), "59s");
    CHECK_OWNED_STR(format_duration_ms(60000), "1m 0s");
    CHECK_OWNED_STR(format_duration_ms(3599000), "59m 59s");
    CHECK_OWNED_STR(format_duration_ms(3600000), "1h 0m");
    CHECK_OWNED_STR(format_duration_ms(3661000), "1h 1m");
    CHECK_OWNED_STR(format_duration_ms(25.0 * 3600000 + 61000), "25h 1m");
}

static void test_negative_durations_and_clocks_are_zero(void) {
    CHECK_OWNED_STR(format_duration_ms(-5000), "0s");
    CHECK_OWNED_STR(format_clock(-5), "0:00");
}

static void test_clock_is_minutes_and_padded_seconds(void) {
    CHECK_OWNED_STR(format_clock(0), "0:00");
    CHECK_OWNED_STR(format_clock(9), "0:09");
    CHECK_OWNED_STR(format_clock(59), "0:59");
    CHECK_OWNED_STR(format_clock(60), "1:00");
    CHECK_OWNED_STR(format_clock(605), "10:05");
    CHECK_OWNED_STR(format_clock(3600), "60:00");
}

// MARK: - Numbers

static void test_tokens_abbreviate_as_the_dashboard_does(void) {
    CHECK_OWNED_STR(format_tokens(0), "0");
    CHECK_OWNED_STR(format_tokens(999), "999");
    CHECK_OWNED_STR(format_tokens(999.9), "999");
    CHECK_OWNED_STR(format_tokens(1000), "1.0k");
    CHECK_OWNED_STR(format_tokens(42900), "42.9k");
    CHECK_OWNED_STR(format_tokens(1e6), "1.0M");
    CHECK_OWNED_STR(format_tokens(19.1e6), "19.1M");
    CHECK_OWNED_STR(format_tokens(839.1e6), "839.1M");
    CHECK_OWNED_STR(format_tokens(1e9), "1.0B");
    CHECK_OWNED_STR(format_tokens(21.6e9), "21.6B");
}

static void test_tokens_that_round_up_to_the_next_unit_take_it(void) {
    CHECK_OWNED_STR(format_tokens(999949), "999.9k");
    CHECK_OWNED_STR(format_tokens(999999), "1.0M");
    CHECK_OWNED_STR(format_tokens(999949999), "999.9M");
    CHECK_OWNED_STR(format_tokens(999999999), "1.0B");
}

static void test_cost_has_a_dollar_sign_and_two_decimals(void) {
    CHECK_OWNED_STR(format_cost(33.35), "$33.35");
    CHECK_OWNED_STR(format_cost(0), "$0.00");
    CHECK_OWNED_STR(format_cost(0.004), "$0.00");
    CHECK_OWNED_STR(format_cost(2), "$2.00");
    CHECK_OWNED_STR(format_cost(1234.5), "$1234.50");
}

static void test_file_sizes_round_up_to_whole_kilobytes(void) {
    CHECK_OWNED_STR(format_file_size(0), "1 KB");
    CHECK_OWNED_STR(format_file_size(1), "1 KB");
    CHECK_OWNED_STR(format_file_size(1023), "1 KB");
    CHECK_OWNED_STR(format_file_size(1024), "1 KB");
    CHECK_OWNED_STR(format_file_size(1025), "2 KB");
    CHECK_OWNED_STR(format_file_size(500 * 1024), "500 KB");
    CHECK_OWNED_STR(format_file_size(1024 * 1024 - 1), "1024 KB");
}

static void test_file_sizes_from_a_megabyte_have_one_decimal(void) {
    CHECK_OWNED_STR(format_file_size(1024 * 1024), "1.0 MB");
    CHECK_OWNED_STR(format_file_size(1024 * 1024 + 1024 * 1024 / 2), "1.5 MB");
    CHECK_OWNED_STR(format_file_size(1024 * 1024 + 1024 * 1024 / 20 - 1), "1.0 MB");
    CHECK_OWNED_STR(format_file_size(25 * 1024 * 1024), "25.0 MB");
}

// MARK: - Colours

static void test_blend_mixes_each_channel_and_clamps_alpha(void) {
    CHECK_INT(blend(RGB(255, 0, 0), RGB(0, 0, 255), 0.5), RGB(128, 0, 128));
    CHECK_INT(blend(RGB(200, 100, 50), RGB(0, 0, 0), 1), RGB(200, 100, 50));
    CHECK_INT(blend(RGB(200, 100, 50), RGB(10, 20, 30), 0), RGB(10, 20, 30));
    CHECK_INT(blend(RGB(200, 100, 50), RGB(10, 20, 30), -1), RGB(10, 20, 30));
    CHECK_INT(blend(RGB(200, 100, 50), RGB(10, 20, 30), 2), RGB(200, 100, 50));
    CHECK_INT(blend(RGB(255, 255, 255), RGB(0, 0, 0), 0.15), RGB(38, 38, 38));
}

static void test_palette_follows_the_app_mode(void) {
    ensure_theme();
    if (theme.dark) {
        CHECK_INT(theme.canvas, hex(0x262624));
        CHECK_INT(theme.sidebar, hex(0x1F1E1D));
        CHECK_INT(theme.raise, hex(0x30302E));
        CHECK_INT(theme.ink, hex(0xE8E6E1));
        CHECK_INT(theme.accent, hex(0xD97757));
        CHECK_INT(theme.on_accent, hex(0x1B1B19));
        CHECK_INT(theme.thumb, hex(0x3C3B38));
    } else {
        CHECK_INT(theme.canvas, hex(0xFAF9F5));
        CHECK_INT(theme.sidebar, hex(0xF0EEE6));
        CHECK_INT(theme.raise, hex(0xFFFFFF));
        CHECK_INT(theme.ink, hex(0x1F1E1D));
        CHECK_INT(theme.accent, hex(0xC96442));
        CHECK_INT(theme.on_accent, hex(0xFFFFFF));
        CHECK_INT(theme.thumb, hex(0xCFCBC0));
    }
    // Text reads on the canvas: light ink on a dark canvas, dark ink on a light one.
    int ink = GetRValue(theme.ink) + GetGValue(theme.ink) + GetBValue(theme.ink);
    int canvas = GetRValue(theme.canvas) + GetGValue(theme.canvas) + GetBValue(theme.canvas);
    CHECK(theme.dark ? ink > canvas : ink < canvas);
}

static void test_palette_aliases_name_the_same_colours(void) {
    ensure_theme();
    CHECK_INT(theme.background, theme.canvas);
    CHECK_INT(theme.surface, theme.raise);
    CHECK_INT(theme.elevated, theme.raise);
    CHECK_INT(theme.bubble, theme.raise);
    CHECK_INT(theme.border, theme.line);
    CHECK_INT(theme.code, theme.sunken);
    CHECK_INT(theme.success, theme.ok);
    CHECK_INT(theme.warning, theme.warn);
    CHECK_INT(theme.text, theme.ink);
    CHECK_INT(theme.secondary, theme.muted);
    CHECK_INT(theme.tertiary, blend(theme.muted, theme.canvas, 0.7));
    CHECK_INT(theme.white, RGB(255, 255, 255));
}

static void test_refresh_reports_a_change_only_once(void) {
    ensure_theme();
    Palette saved = theme;
    CHECK(!theme_refresh());
    CHECK(memcmp(&saved, &theme, sizeof theme) == 0);
    // A palette never filled in counts as a change.
    theme.canvas = 0;
    CHECK(theme_refresh());
    CHECK_INT(theme.canvas, saved.canvas);
    CHECK(!theme_refresh());
    theme = saved;
}

static void test_status_colours_match_the_dashboard_dots(void) {
    ensure_theme();
    const char *accent[] = { "running", "queued", "preparing", "starting" };
    for (size_t i = 0; i < 4; i++) CHECK_INT(theme_status_color(accent[i]), theme.accent);
    CHECK_INT(theme_status_color("idle"), theme.ok);
    const char *warn[] = { "waiting", "interrupted", "cancelled" };
    for (size_t i = 0; i < 3; i++) CHECK_INT(theme_status_color(warn[i]), theme.warn);
    CHECK_INT(theme_status_color("failed"), theme.danger);
    CHECK_INT(theme_status_color("error"), theme.danger);
    CHECK_INT(theme_status_color("closed"), theme.dot);
    CHECK_INT(theme_status_color("Running"), theme.dot);
    CHECK_INT(theme_status_color(""), theme.dot);
    CHECK_INT(theme_status_color(NULL), theme.dot);
}

static void test_status_colours_follow_a_swapped_palette(void) {
    ensure_theme();
    Palette saved = theme;
    theme.accent = RGB(1, 0, 0); theme.ok = RGB(2, 0, 0); theme.warn = RGB(3, 0, 0); theme.danger = RGB(4, 0, 0); theme.dot = RGB(5, 0, 0);
    CHECK_INT(theme_status_color("running"), RGB(1, 0, 0));
    CHECK_INT(theme_status_color("idle"), RGB(2, 0, 0));
    CHECK_INT(theme_status_color("cancelled"), RGB(3, 0, 0));
    CHECK_INT(theme_status_color("error"), RGB(4, 0, 0));
    CHECK_INT(theme_status_color("archived"), RGB(5, 0, 0));
    theme = saved;
}

// MARK: - Scaling

static void test_px_scales_with_the_dpi(void) {
    ensure_theme();
    CHECK_INT(theme_dpi(), 96);
    CHECK_INT(px(10), 10);
    int body = font_height(NULL, FONT_BODY);
    theme_set_dpi(144);
    CHECK_INT(theme_dpi(), 144);
    CHECK_INT(px(10), 15);
    CHECK_INT(px(7), 11);
    CHECK_INT(px(-4), -6);
    CHECK_INT(px(0), 0);
    theme_set_dpi(192);
    CHECK_INT(px(13), 26);
    CHECK(font_height(NULL, FONT_BODY) > body);
    theme_set_dpi(96);
    CHECK_INT(font_height(NULL, FONT_BODY), body);
}

static void test_an_unusable_dpi_falls_back_to_96(void) {
    ensure_theme();
    theme_set_dpi(0);
    CHECK_INT(theme_dpi(), 96);
    CHECK_INT(px(12), 12);
    theme_set_dpi(-120);
    CHECK_INT(theme_dpi(), 96);
    theme_set_dpi(96);
}

// MARK: - Working indicator

static void test_working_glyph_cycles_every_ten_ticks(void) {
    CHECK(wcscmp(working_glyph(0), L"\u00B7") == 0);
    CHECK(wcscmp(working_glyph(1), L"\u2722") == 0);
    CHECK(wcscmp(working_glyph(4), L"\u273B") == 0);
    CHECK(wcscmp(working_glyph(5), L"\u273D") == 0);
    CHECK(wcscmp(working_glyph(9), L"\u2722") == 0);
    CHECK(wcscmp(working_glyph(10), working_glyph(0)) == 0);
    CHECK(wcscmp(working_glyph(1234567), working_glyph(7)) == 0);
}

static void test_working_glyph_wraps_negative_ticks(void) {
    CHECK(wcscmp(working_glyph(-1), working_glyph(9)) == 0);
    CHECK(wcscmp(working_glyph(-10), working_glyph(0)) == 0);
    CHECK(wcscmp(working_glyph(-13), working_glyph(7)) == 0);
    CHECK(working_glyph(-2147483647 - 1) != NULL);
}

static void test_working_verb_changes_every_25_ticks(void) {
    CHECK_STR(working_verb(0), "Working");
    CHECK_STR(working_verb(24), "Working");
    CHECK_STR(working_verb(25), "Thinking");
    CHECK_STR(working_verb(50), "Reasoning");
    CHECK_STR(working_verb(75), "Tinkering");
    CHECK_STR(working_verb(100), "Crafting");
    CHECK_STR(working_verb(125), "Pondering");
    CHECK_STR(working_verb(150), "Working");
}

static void test_working_verb_wraps_negative_ticks(void) {
    // Division truncates toward zero, so -24...24 all read "Working".
    CHECK_STR(working_verb(-24), "Working");
    CHECK_STR(working_verb(-25), "Pondering");
    CHECK_STR(working_verb(-150), "Working");
    CHECK(working_verb(-2147483647 - 1) != NULL);
}

void app_format_tests(void) {
    test_run("event time today is the clock alone", test_event_time_today_is_the_clock_alone);
    test_run("event time on another day names the day", test_event_time_on_another_day_names_the_day);
    test_run("event times take the offset of their own date", test_event_times_take_the_offset_of_their_own_date);
    test_run("date abbrev is month, day and year", test_date_abbrev_is_month_day_and_year);
    test_run("relative time steps from minutes to years", test_relative_time_steps_from_minutes_to_years);
    test_run("relative time in the future has no ago", test_relative_time_in_the_future_has_no_ago);
    test_run("duration shows the two largest units", test_duration_shows_the_two_largest_units);
    test_run("negative durations and clocks are zero", test_negative_durations_and_clocks_are_zero);
    test_run("clock is minutes and padded seconds", test_clock_is_minutes_and_padded_seconds);
    test_run("tokens abbreviate as the dashboard does", test_tokens_abbreviate_as_the_dashboard_does);
    test_run("tokens that round up to the next unit take it", test_tokens_that_round_up_to_the_next_unit_take_it);
    test_run("cost has a dollar sign and two decimals", test_cost_has_a_dollar_sign_and_two_decimals);
    test_run("file sizes round up to whole kilobytes", test_file_sizes_round_up_to_whole_kilobytes);
    test_run("file sizes from a megabyte have one decimal", test_file_sizes_from_a_megabyte_have_one_decimal);
    test_run("blend mixes each channel and clamps alpha", test_blend_mixes_each_channel_and_clamps_alpha);
    test_run("palette follows the app mode", test_palette_follows_the_app_mode);
    test_run("palette aliases name the same colours", test_palette_aliases_name_the_same_colours);
    test_run("refresh reports a change only once", test_refresh_reports_a_change_only_once);
    test_run("status colours match the dashboard dots", test_status_colours_match_the_dashboard_dots);
    test_run("status colours follow a swapped palette", test_status_colours_follow_a_swapped_palette);
    test_run("px scales with the dpi", test_px_scales_with_the_dpi);
    test_run("an unusable dpi falls back to 96", test_an_unusable_dpi_falls_back_to_96);
    test_run("working glyph cycles every ten ticks", test_working_glyph_cycles_every_ten_ticks);
    test_run("working glyph wraps negative ticks", test_working_glyph_wraps_negative_ticks);
    test_run("working verb changes every 25 ticks", test_working_verb_changes_every_25_ticks);
    test_run("working verb wraps negative ticks", test_working_verb_wraps_negative_ticks);
}
